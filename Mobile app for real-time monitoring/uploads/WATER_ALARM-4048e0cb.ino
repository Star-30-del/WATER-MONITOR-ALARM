#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// ===== TELEGRAM CONFIG =====
const char* bot_token = "8648529966:AAEgrg85mQ4kxmNzgy5C9tiesQdENSdCTrw";
const char* chat_id = "-5181401248";

// ===== NTP CONFIG =====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;
bool morningNotifySent = false;

// ===== GPIO =====
const int pinSensor    = 34; // ใช้พิน Analog เช่น GPIO34 (ADC1)
const int pinLedWifi   = 23;
const int pinResetWifi = 13;

// ===== ADC CALIBRATION =====
// ค่า ADC ที่กระแส 4mA (0%) และ 20mA (100%) *ต้อง Calibrate หน้างานจริง*
const int adcAtMin   = 745;   // 4mA  -> 0%
const int adcAtMax   = 3723;  // 20mA -> 100%
const int adcSamples = 16;    // จำนวนครั้งที่เฉลี่ยเพื่อกรอง noise

// ===== ALERT THRESHOLDS (ตั้งค่าผ่านหน้าเว็บได้ / เก็บถาวรใน NVS) =====
// ค่าเริ่มต้น ถ้ายังไม่เคยตั้งค่าผ่านเว็บ
int highPct = 75;  // >= ค่านี้ = ระดับสูง
int medPct  = 50;  // >= ค่านี้ = ระดับกลาง
int lowPct  = 25;  // <  ค่านี้ = ระดับต่ำ
int hystPct = 3;   // ระยะกันสั่น (Hysteresis) เป็น %

// ===== State =====
unsigned long lastReconnectTry = 0;
unsigned long lastWaterCheck = 0;
bool wifiWasConnected = false;
float lastWaterPercent = 0;   // ค่าล่าสุด (ไว้โชว์บนหน้าเว็บ)

// ระดับปัจจุบัน (band): 3=สูง, 2=กลาง, 1=ปกติ(ไม่แจ้ง), 0=ต่ำ ; -1 = ยังไม่ init
int  currentBand = -1;
bool bandInitialized = false;

Preferences prefs;
WebServer server(80);

// ================= CONFIG STORAGE =================
void loadConfig() {
  prefs.begin("wateralarm", true); // read-only
  highPct = prefs.getInt("high", highPct);
  medPct  = prefs.getInt("med",  medPct);
  lowPct  = prefs.getInt("low",  lowPct);
  hystPct = prefs.getInt("hyst", hystPct);
  prefs.end();
}

void saveConfig() {
  prefs.begin("wateralarm", false); // read-write
  prefs.putInt("high", highPct);
  prefs.putInt("med",  medPct);
  prefs.putInt("low",  lowPct);
  prefs.putInt("hyst", hystPct);
  prefs.end();
}

// ================= TELEGRAM PUSH =================
void sendTelegramMessage(String message) {
  if (!WiFi.isConnected()) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);

  String host = "api.telegram.org";
  if (!client.connect(host.c_str(), 443)) {
    Serial.println("Telegram connect fail");
    return;
  }

  String url = "/bot" + String(bot_token) + "/sendMessage";
  String payload = "{\"chat_id\":\"" + String(chat_id) + "\",\"text\":\"" + message + "\"}";

  client.print(String("POST ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + payload.length() + "\r\n" +
               "Connection: close\r\n\r\n" +
               payload);

  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 3000) {
    while (client.available()) {
      client.read();
    }
  }
  client.stop();
  Serial.println("Telegram Message Sent: " + message);
}

// ================= ADC READ (AVERAGED) =================
// อ่านค่า ADC หลายครั้งแล้วเฉลี่ย เพื่อกรองสัญญาณรบกวน (noise) ของ 4-20mA
int readSensorAveraged() {
  long sum = 0;
  for (int i = 0; i < adcSamples; i++) {
    sum += analogRead(pinSensor);
    delay(2);
  }
  return (int)(sum / adcSamples);
}

// ================= BAND DECISION (Hysteresis) =================
// ตัดสินว่าน้ำอยู่ระดับไหน โดยกันการสวิงที่ขอบเกณฑ์:
//  - ขาขึ้น ใช้เกณฑ์ปกติ (highPct/medPct/lowPct)
//  - ขาลง ต้องต่ำกว่าเกณฑ์เกิน hystPct ถึงจะลดระดับ
int decideBand(float w, int cur) {
  // แบนด์ตามเกณฑ์ปกติ (สำหรับขาขึ้น)
  int up;
  if      (w >= highPct) up = 3;
  else if (w >= medPct)  up = 2;
  else if (w >= lowPct)  up = 1;
  else                   up = 0;

  // แบนด์ตามเกณฑ์ที่ลดลง hystPct (สำหรับขาลง)
  int down;
  if      (w >= highPct - hystPct) down = 3;
  else if (w >= medPct  - hystPct) down = 2;
  else if (w >= lowPct  - hystPct) down = 1;
  else                             down = 0;

  if (up   > cur) return up;    // น้ำเพิ่มขึ้นข้ามเกณฑ์ -> ขึ้นระดับ
  if (down < cur) return down;  // น้ำลดลงต่ำกว่าเกณฑ์เกิน hyst -> ลดระดับ
  return cur;                   // อยู่ในโซนกันสั่น -> คงระดับเดิม
}

// ================= BAND MESSAGE =================
String bandMessage(int band, float w) {
  String cur = " (ปัจจุบัน " + String(w, 1) + "%)";
  switch (band) {
    case 3: return "🔴 แจ้งเตือน: ระดับน้ำสูง ≥ " + String(highPct) + "%" + cur;
    case 2: return "🟡 แจ้งเตือน: ระดับน้ำ ≥ " + String(medPct) + "%" + cur;
    case 0: return "🔵 แจ้งเตือน: ระดับน้ำต่ำกว่า " + String(lowPct) + "%" + cur;
    default: return ""; // band 1 (ปกติ) ไม่แจ้งเตือน
  }
}

// ================= WATER CHECK (ANALOG 4-20mA) =================
void checkWaterLevelAnalog() {
  int analogValue = readSensorAveraged();

  // แปลงเป็นเปอร์เซ็นต์ด้วยเลขทศนิยมจริง (ไม่ใช้ map() ที่ปัดเป็นจำนวนเต็ม)
  float waterLevelPercent =
      (float)(analogValue - adcAtMin) * 100.0f / (float)(adcAtMax - adcAtMin);

  if (waterLevelPercent < 0)   waterLevelPercent = 0;
  if (waterLevelPercent > 100) waterLevelPercent = 100;
  lastWaterPercent = waterLevelPercent;

  // ตัดสินระดับปัจจุบัน แล้วแจ้งเฉพาะตอน "เปลี่ยนระดับ" เท่านั้น
  int newBand = decideBand(waterLevelPercent, currentBand < 0 ? 0 : currentBand);

  if (!bandInitialized) {
    // ครั้งแรกหลังบูต: ตั้งค่าฐานเงียบๆ ไม่ส่งแจ้งเตือน
    currentBand = newBand;
    bandInitialized = true;
  } else if (newBand != currentBand) {
    String msg = bandMessage(newBand, waterLevelPercent);
    if (msg.length() > 0) sendTelegramMessage(msg);
    currentBand = newBand;
  }
}

// ================= WEB CONFIG PAGE =================
String htmlPage(String notice) {
  String bandName;
  switch (currentBand) {
    case 3: bandName = "สูง 🔴"; break;
    case 2: bandName = "กลาง 🟡"; break;
    case 1: bandName = "ปกติ ⚪"; break;
    case 0: bandName = "ต่ำ 🔵"; break;
    default: bandName = "-";
  }

  String h;
  h += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>ตั้งค่าแจ้งเตือนระดับน้ำ</title>";
  h += "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 16px;}"
       "h2{color:#0a58ca}label{display:block;margin:14px 0 4px;font-weight:bold}"
       "input{width:100%;padding:10px;font-size:16px;box-sizing:border-box}"
       "button{margin-top:20px;width:100%;padding:12px;font-size:16px;background:#0a58ca;"
       "color:#fff;border:0;border-radius:6px}.box{background:#f2f4f8;padding:12px;border-radius:8px}"
       ".ok{background:#d1e7dd;color:#0f5132;padding:10px;border-radius:6px}"
       ".err{background:#f8d7da;color:#842029;padding:10px;border-radius:6px}</style></head><body>";
  h += "<h2>ตั้งค่าแจ้งเตือนระดับน้ำ</h2>";
  if (notice.length() > 0) h += notice;
  h += "<div class='box'>ระดับน้ำปัจจุบัน: <b>" + String(lastWaterPercent, 1) + "%</b><br>";
  h += "สถานะ: <b>" + bandName + "</b></div>";
  h += "<form action='/save' method='POST'>";
  h += "<label>ระดับสูง (High %)</label><input type='number' name='high' min='1' max='100' value='" + String(highPct) + "'>";
  h += "<label>ระดับกลาง (Medium %)</label><input type='number' name='med' min='1' max='100' value='" + String(medPct) + "'>";
  h += "<label>ระดับต่ำ (Low %)</label><input type='number' name='low' min='0' max='100' value='" + String(lowPct) + "'>";
  h += "<label>ระยะกันสั่น (Hysteresis %)</label><input type='number' name='hyst' min='0' max='20' value='" + String(hystPct) + "'>";
  h += "<button type='submit'>บันทึก</button></form>";
  h += "<p style='color:#888;font-size:13px'>เงื่อนไข: ต่ำ &lt; กลาง &lt; สูง (0-100)</p>";
  h += "</body></html>";
  return h;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage(""));
}

void handleSave() {
  int h = server.arg("high").toInt();
  int m = server.arg("med").toInt();
  int l = server.arg("low").toInt();
  int y = server.arg("hyst").toInt();

  // ตรวจสอบความถูกต้อง: 0 <= low < med < high <= 100
  if (l < 0 || h > 100 || !(l < m && m < h) || y < 0 || y > 20) {
    server.send(200, "text/html; charset=utf-8",
      htmlPage("<div class='err'>ค่าไม่ถูกต้อง: ต้องเป็น ต่ำ &lt; กลาง &lt; สูง (0-100) และ hyst 0-20</div>"));
    return;
  }

  highPct = h; medPct = m; lowPct = l; hystPct = y;
  saveConfig();

  // รีเซ็ต baseline เพื่อประเมินระดับใหม่ตามเกณฑ์ล่าสุด (เงียบ ไม่สแปม)
  bandInitialized = false;
  currentBand = -1;

  server.send(200, "text/html; charset=utf-8",
    htmlPage("<div class='ok'>บันทึกเรียบร้อย ✅</div>"));
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// ================= MORNING ONLINE CHECK =================
void checkMorningOnline() {
  if (!WiFi.isConnected()) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;

  if (hour == 8 && minute == 0 && !morningNotifySent) {
    sendTelegramMessage("☀️ รายงานสถานะ 08:00 น. ระบบกำลังออนไลน์ปกติ");
    morningNotifySent = true;
  }

  if (hour > 8) {
    morningNotifySent = false;
  }
}

// ================= RESET BUTTON =================
void checkResetButton() {
  static unsigned long pressStart = 0;

  if (digitalRead(pinResetWifi) == LOW) {
    if (pressStart == 0) pressStart = millis();

    if (millis() - pressStart > 5000) {
      digitalWrite(pinLedWifi, LOW);
      WiFi.disconnect(true, true);
      delay(1000);

      WiFiManager wm;
      wm.resetSettings();

      delay(1000);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

// ================= WIFI START =================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("WaterLevel_Setup")) {
    ESP.restart();
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  wifiWasConnected = true;

  String ip = WiFi.localIP().toString();
  Serial.println("Web config: http://" + ip);
  sendTelegramMessage("✅ ระบบ Online\nตั้งค่าแจ้งเตือนที่ http://" + ip);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12); // ADC 12-bit (0-4095)

  pinMode(pinLedWifi, OUTPUT);
  pinMode(pinResetWifi, INPUT_PULLUP);

  loadConfig();      // โหลดค่าเกณฑ์ที่เคยตั้งไว้จาก NVS
  startWiFi();
  setupWebServer();  // เปิดหน้าเว็บตั้งค่า
}

// ================= LOOP =================
void loop() {
  digitalWrite(pinLedWifi, WiFi.isConnected());
  server.handleClient();

  // WiFi Monitor
  if (!WiFi.isConnected()) {
    if (wifiWasConnected) wifiWasConnected = false;
    if (millis() - lastReconnectTry > 15000) {
      lastReconnectTry = millis();
      WiFi.disconnect();
      WiFi.begin();
    }
  } else {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      sendTelegramMessage("✅ WiFi กลับมาแล้ว");
    }
  }

  // เช็กระดับน้ำทุก 1 วินาที (ไม่บล็อกเว็บเซิร์ฟเวอร์)
  if (millis() - lastWaterCheck > 1000) {
    lastWaterCheck = millis();
    checkWaterLevelAnalog();
  }

  checkMorningOnline();
  checkResetButton();
}

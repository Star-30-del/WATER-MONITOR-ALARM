#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>

// ===== TELEGRAM CONFIG =====
// นำ API Token จาก @BotFather และ Chat ID จาก @userinfobot มาใส่ที่นี่
const char* bot_token = "8648529966:AAEgrg85mQ4kxmNzgy5C9tiesQdENSdCTrw";
const char* chat_id = "-5181401248";

// ===== NTP CONFIG (Thailand GMT+7) =====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;
bool morningNotifySent = false;

// ===== GPIO =====
const int pinHigh      = 18;
const int pinMedium    = 17;
const int pinLow       = 16;
const int pinLedWifi   = 23;
const int pinResetWifi = 13;

// ===== Messages =====
String msgHigh   = "แจ้งเตือน: ระดับน้ำในถัง 75%";
String msgMedium = "แจ้งเตือน: ระดับน้ำในถัง 50%";
String msgLow    = "แจ้งเตือน: ระดับน้ำในถังต่ำกว่า 25%";

bool lastHigh = false;
bool lastMedium = false;
bool lastLow = false;
bool wifiWasConnected = false;

unsigned long lastCheck = 0;
unsigned long lastReconnectTry = 0;

// ================= TELEGRAM PUSH =================
void sendTelegramMessage(String message) {

  // ตรวจสอบว่าต่อเน็ตอยู่หรือไม่
  if (!WiFi.isConnected()) return;

  WiFiClientSecure client;
  client.setInsecure(); // ข้ามการตรวจสอบ SSL Certificate
  client.setTimeout(5000);

  String host = "api.telegram.org";
  if (!client.connect(host.c_str(), 443)) {
    Serial.println("Telegram connect fail");
    return;
  }

  // สร้าง URL และ Payload ในรูปแบบ JSON ของ Telegram
  String url = "/bot" + String(bot_token) + "/sendMessage";
  String payload = "{\"chat_id\":\"" + String(chat_id) + "\",\"text\":\"" + message + "\"}";

  // ส่ง HTTP POST Request
  client.print(String("POST ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + payload.length() + "\r\n" +
               "Connection: close\r\n\r\n" +
               payload);

  // รออ่าน response กลับมาเพื่อไม่ให้การเชื่อมต่อค้าง
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 3000) {
    while (client.available()) {
      client.read();
    }
  }

  client.stop();
  Serial.println("Telegram Message Sent: " + message);
}

// ================= STABLE READ =================
bool readStable(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(150);
    return digitalRead(pin) == LOW;
  }
  return false;
}

// ================= WATER CHECK =================
void checkWaterLevel() {

  bool h = readStable(pinHigh);
  bool m = readStable(pinMedium);
  bool l = readStable(pinLow);

  if (h && !lastHigh) sendTelegramMessage(msgHigh);
  if (m && !lastMedium) sendTelegramMessage(msgMedium);
  if (l && !lastLow) sendTelegramMessage(msgLow);

  lastHigh = h;
  lastMedium = m;
  lastLow = l;
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
  sendTelegramMessage("✅ ระบบ Online");
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);
  delay(1000);

  pinMode(pinHigh, INPUT_PULLUP);
  pinMode(pinMedium, INPUT_PULLUP);
  pinMode(pinLow, INPUT_PULLUP);
  pinMode(pinLedWifi, OUTPUT);
  pinMode(pinResetWifi, INPUT_PULLUP);

  startWiFi();
}

// ================= LOOP =================
void loop() {

  // LED Status
  digitalWrite(pinLedWifi, WiFi.isConnected());

  // WiFi Monitor
  if (!WiFi.isConnected()) {

    if (wifiWasConnected) {
      wifiWasConnected = false;
      // หมายเหตุ: โค้ดบรรทัดล่างนี้จะไม่ถูกส่งไปยัง Telegram ทันทีเพราะไม่มีเน็ต
      // แต่จะถูกข้ามไปเนื่องจากฟังก์ชัน sendTelegramMessage ตรวจสอบว่า !WiFi.isConnected() ไว้แล้ว
    }

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

  // Water Level
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    checkWaterLevel();
  }

  // Morning Online Report
  checkMorningOnline();

  // Reset Button
  checkResetButton();
}
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
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
const int   adcAtMin   = 745;   // 4mA  -> 0%
const int   adcAtMax   = 3723;  // 20mA -> 100%
const int   adcSamples = 16;    // จำนวนครั้งที่เฉลี่ยเพื่อกรอง noise

// ===== Messages =====
String msgHigh   = "แจ้งเตือน: ระดับน้ำในถัง 75%";
String msgMedium = "แจ้งเตือน: ระดับน้ำในถัง 50%";
String msgLow    = "แจ้งเตือน: ระดับน้ำในถังต่ำกว่า 25%";

// ===== Real-time & State Variables =====
unsigned long lastReconnectTry = 0;
bool wifiWasConnected = false;

// ตัวแปรเก็บสถานะการแจ้งเตือน (ส่งครั้งเดียวเมื่อถึงระดับ)
bool sentHigh = false;
bool sentMedium = false;
bool sentLow = false;

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
// อ่านค่า ADC หลายครั้งแล้วเฉลี่ย เพื่อกรองสัญญาณรบกวน (noise)
// ของสัญญาณ 4-20mA ไม่ให้ค่าเด้งข้ามเกณฑ์จนแจ้งเตือนซ้ำ
int readSensorAveraged() {
  long sum = 0;
  for (int i = 0; i < adcSamples; i++) {
    sum += analogRead(pinSensor);
    delay(2);
  }
  return (int)(sum / adcSamples);
}

// ================= WATER CHECK (ANALOG 4-20mA) =================
void checkWaterLevelAnalog() {
  // อ่านค่า ADC แบบเฉลี่ยจาก ESP32 (0 ถึง 4095)
  int analogValue = readSensorAveraged();

  // แปลงค่าอนาล็อกเป็นเปอร์เซ็นต์ (0 - 100%) ด้วยเลขทศนิยมจริง
  // (ไม่ใช้ map() เพราะ map() คำนวณเป็นจำนวนเต็ม ทำให้ทศนิยมหาย)
  float waterLevelPercent =
      (float)(analogValue - adcAtMin) * 100.0f / (float)(adcAtMax - adcAtMin);

  // ป้องกันค่าหลุดช่วง 0-100%
  if(waterLevelPercent < 0) waterLevelPercent = 0;
  if(waterLevelPercent > 100) waterLevelPercent = 100;

  // --- ตรรกะเช็กระดับน้ำพร้อมกันการสวิง (Hysteresis) ---

  // ระดับ High (>= 75%)
  if (waterLevelPercent >= 75) {
    if (!sentHigh) {
      sendTelegramMessage(msgHigh + " (ปัจจุบัน " + String(waterLevelPercent, 1) + "%)");
      sentHigh = true;
    }
  } else if (waterLevelPercent < 72) { // ถ้าน้ำลดลงต่ำกว่า 72% ถึงจะรีเซ็ตสถานะเพื่อให้แจ้งเตือนใหม่ได้
    sentHigh = false;
  }

  // ระดับ Medium (>= 50% และยังไม่ถึง 75%)
  if (waterLevelPercent >= 50 && waterLevelPercent < 75) {
    if (!sentMedium) {
      sendTelegramMessage(msgMedium + " (ปัจจุบัน " + String(waterLevelPercent, 1) + "%)");
      sentMedium = true;
    }
  } else if (waterLevelPercent < 47 || waterLevelPercent >= 75) {
    sentMedium = false;
  }

  // ระดับ Low (< 25%)
  if (waterLevelPercent <= 25) {
    if (!sentLow) {
      sendTelegramMessage(msgLow + " (ปัจจุบัน " + String(waterLevelPercent, 1) + "%)");
      sentLow = true;
    }
  } else if (waterLevelPercent > 28) {
    sentLow = false;
  }
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

  // ตั้งค่า ADC ความละเอียด 12-bit (0-4095) และลดสัญญาณรบกวน
  analogReadResolution(12);

  pinMode(pinLedWifi, OUTPUT);
  pinMode(pinResetWifi, INPUT_PULLUP);

  startWiFi();
}

// ================= LOOP =================
void loop() {
  digitalWrite(pinLedWifi, WiFi.isConnected());

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

  // อ่านค่าอนาล็อกและเช็กระดับน้ำแบบ Real-time ทันที
  checkWaterLevelAnalog();

  checkMorningOnline();
  checkResetButton();
}

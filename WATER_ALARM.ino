#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
// Cloud database — Library Manager: "Firebase Arduino Client Library for ESP8266 and ESP32" (by Mobizt)
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ================= NTP =================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

// ================= GPIO =================
const int pinSensor    = 34; // Analog 4-20mA (ADC1)
const int pinLedWifi   = 23;
const int pinResetWifi = 13;
const int adcSamples   = 16; // เฉลี่ยกรอง noise

// ================= CONFIG (persisted in NVS) =================
int    highPct = 75, medPct = 50, lowPct = 25, hystPct = 3;
int    adcMin = 745, adcMax = 3723;
String msgHigh   = "ระดับน้ำสูง";
String msgMedium = "ระดับน้ำปานกลาง";
String msgLow    = "ระดับน้ำต่ำ กรุณาเติมน้ำ";
bool   morningEnabled = true;
int    morningHour = 8;
String msgMorning = "ระบบกำลังออนไลน์ปกติ";
String botToken = "8648529966:AAEgrg85mQ4kxmNzgy5C9tiesQdENSdCTrw";
String chatId   = "-5181401248";

// ---- Firebase Realtime Database (so the mobile app can reach the device from anywhere) ----
// Fill these from your Firebase project (Project settings + Realtime Database).
// The mobile app points at the SAME project + SAME deviceId.
//   - fbApiKey / fbDatabaseUrl : from the firebaseConfig object
//   - fbUserEmail / fbUserPass : an Authentication user (Email/Password). Leave BOTH empty
//     to sign in anonymously (then RTDB rules must allow auth != null or be in test mode).
bool   fbEnabled     = false;
String fbApiKey      = "";
String fbDatabaseUrl = "";             // e.g. "https://xxxx-default-rtdb.firebasedatabase.app"
String fbUserEmail   = "";
String fbUserPass    = "";
String deviceId      = "wateralarm-1"; // must match the "Device ID" set in the app

// ================= RUNTIME =================
float lastWaterPercent = 0;
int   lastAdc = 0;
int   currentBand = -1;
bool  bandInitialized = false;
bool  morningNotifySent = false;
unsigned long lastReconnectTry = 0, lastWaterCheck = 0, lastHistSample = 0;
bool  wifiWasConnected = false;

#define HIST_SIZE 48
uint8_t histBuf[HIST_SIZE];
int histCount = 0;

#define LOG_SIZE 20
String logLevel[LOG_SIZE], logMsg[LOG_SIZE], logTime[LOG_SIZE];
int logN = 0;

Preferences prefs;
WebServer server(80);

// Firebase RTDB
FirebaseData fbdo;          // read/write + command polling
FirebaseAuth fbAuth;
FirebaseConfig fbCfg;
bool  fbInited = false;
unsigned long lastFbPublish = 0, lastFbCmdPoll = 0;

// ================= CONFIG STORAGE =================
void loadConfig() {
  prefs.begin("wateralarm", true);
  highPct = prefs.getInt("high", highPct);
  medPct  = prefs.getInt("med",  medPct);
  lowPct  = prefs.getInt("low",  lowPct);
  hystPct = prefs.getInt("hyst", hystPct);
  adcMin  = prefs.getInt("adcMin", adcMin);
  adcMax  = prefs.getInt("adcMax", adcMax);
  msgHigh   = prefs.getString("msgHigh", msgHigh);
  msgMedium = prefs.getString("msgMed", msgMedium);
  msgLow    = prefs.getString("msgLow", msgLow);
  morningEnabled = prefs.getBool("mEn", morningEnabled);
  morningHour    = prefs.getInt("mHour", morningHour);
  msgMorning     = prefs.getString("msgMorn", msgMorning);
  botToken = prefs.getString("token", botToken);
  chatId   = prefs.getString("chatId", chatId);
  fbEnabled     = prefs.getBool("fbEn", fbEnabled);
  fbApiKey      = prefs.getString("fbKey", fbApiKey);
  fbDatabaseUrl = prefs.getString("fbUrl", fbDatabaseUrl);
  fbUserEmail   = prefs.getString("fbEmail", fbUserEmail);
  fbUserPass    = prefs.getString("fbPass", fbUserPass);
  deviceId      = prefs.getString("devId", deviceId);
  prefs.end();
}

void saveConfig() {
  prefs.begin("wateralarm", false);
  prefs.putInt("high", highPct);
  prefs.putInt("med",  medPct);
  prefs.putInt("low",  lowPct);
  prefs.putInt("hyst", hystPct);
  prefs.putInt("adcMin", adcMin);
  prefs.putInt("adcMax", adcMax);
  prefs.putString("msgHigh", msgHigh);
  prefs.putString("msgMed",  msgMedium);
  prefs.putString("msgLow",  msgLow);
  prefs.putBool("mEn", morningEnabled);
  prefs.putInt("mHour", morningHour);
  prefs.putString("msgMorn", msgMorning);
  prefs.putString("token", botToken);
  prefs.putString("chatId", chatId);
  prefs.putBool("fbEn", fbEnabled);
  prefs.putString("fbKey", fbApiKey);
  prefs.putString("fbUrl", fbDatabaseUrl);
  prefs.putString("fbEmail", fbUserEmail);
  prefs.putString("fbPass", fbUserPass);
  prefs.putString("devId", deviceId);
  prefs.end();
}

// ================= HELPERS =================
String nowHHMM() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char b[6];
  sprintf(b, "%02d:%02d", t.tm_hour, t.tm_min);
  return String(b);
}

String jsonEscape(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:   o += c;
    }
  }
  return o;
}

void addLog(const char* level, const String& msg) {
  if (logN < LOG_SIZE) {
    logLevel[logN] = level; logMsg[logN] = msg; logTime[logN] = nowHHMM(); logN++;
  } else {
    for (int i = 1; i < LOG_SIZE; i++) {
      logLevel[i-1] = logLevel[i]; logMsg[i-1] = logMsg[i]; logTime[i-1] = logTime[i];
    }
    logLevel[LOG_SIZE-1] = level; logMsg[LOG_SIZE-1] = msg; logTime[LOG_SIZE-1] = nowHHMM();
  }
}

void pushHistory(int v) {
  if (v < 0) v = 0; if (v > 100) v = 100;
  if (histCount < HIST_SIZE) {
    histBuf[histCount++] = v;
  } else {
    for (int i = 1; i < HIST_SIZE; i++) histBuf[i-1] = histBuf[i];
    histBuf[HIST_SIZE-1] = v;
  }
}

// ================= TELEGRAM =================
bool sendTelegramMessage(String message) {
  if (!WiFi.isConnected()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);

  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("Telegram connect fail");
    return false;
  }

  String url = "/bot" + botToken + "/sendMessage";
  String payload = "{\"chat_id\":\"" + chatId + "\",\"text\":\"" + jsonEscape(message) + "\"}";

  client.print(String("POST ") + url + " HTTP/1.1\r\n" +
               "Host: api.telegram.org\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + payload.length() + "\r\n" +
               "Connection: close\r\n\r\n" + payload);

  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 3000) {
    while (client.available()) client.read();
  }
  client.stop();
  Serial.println("Telegram Sent: " + message);
  return true;
}

// ================= SENSOR =================
int readSensorAveraged() {
  long sum = 0;
  for (int i = 0; i < adcSamples; i++) { sum += analogRead(pinSensor); delay(2); }
  return (int)(sum / adcSamples);
}

// ตัดสิน band ด้วย hysteresis: ขาขึ้นใช้เกณฑ์ปกติ, ขาลงต้องต่ำกว่าเกณฑ์เกิน hystPct
int decideBand(float w, int cur) {
  int up   = (w >= highPct) ? 3 : (w >= medPct) ? 2 : (w >= lowPct) ? 1 : 0;
  int down = (w >= highPct - hystPct) ? 3 : (w >= medPct - hystPct) ? 2 : (w >= lowPct - hystPct) ? 1 : 0;
  if (up   > cur) return up;
  if (down < cur) return down;
  return cur;
}

void notifyBand(int band, float w) {
  String cur = " (ปัจจุบัน " + String(w, 0) + "%)";
  String level, text;
  if      (band == 3) { level = "high";   text = "🔴 " + msgHigh   + cur; }
  else if (band == 2) { level = "medium"; text = "🟡 " + msgMedium + cur; }
  else if (band == 0) { level = "low";    text = "🔵 " + msgLow    + cur; }
  else return; // band 1 (ปกติ) ไม่แจ้ง
  sendTelegramMessage(text);
  addLog(level.c_str(), text);
}

void checkWaterLevelAnalog() {
  lastAdc = readSensorAveraged();
  float p = (float)(lastAdc - adcMin) * 100.0f / (float)(adcMax - adcMin);
  if (p < 0) p = 0; if (p > 100) p = 100;
  lastWaterPercent = p;

  int newBand = decideBand(p, currentBand < 0 ? 0 : currentBand);
  if (!bandInitialized) {
    currentBand = newBand; bandInitialized = true;
  } else if (newBand != currentBand) {
    notifyBand(newBand, p);
    currentBand = newBand;
  }
}

// ================= JSON STATUS (shared by HTTP + Firebase) =================
String buildStatusJson() {
  float ma = 4.0f + (lastWaterPercent / 100.0f) * 16.0f;
  String j = "{";
  j += "\"percent\":" + String(lastWaterPercent, 1) + ",";
  j += "\"band\":" + String(currentBand) + ",";
  j += "\"wifi\":" + String(WiFi.isConnected() ? "true" : "false") + ",";
  j += "\"lastUpdate\":\"" + nowHHMM() + "\",";
  j += "\"high\":" + String(highPct) + ",\"med\":" + String(medPct) + ",\"low\":" + String(lowPct) + ",\"hyst\":" + String(hystPct) + ",";
  j += "\"adcMin\":" + String(adcMin) + ",\"adcMax\":" + String(adcMax) + ",\"adc\":" + String(lastAdc) + ",\"ma\":" + String(ma, 1) + ",";
  j += "\"msgHigh\":\"" + jsonEscape(msgHigh) + "\",\"msgMedium\":\"" + jsonEscape(msgMedium) + "\",\"msgLow\":\"" + jsonEscape(msgLow) + "\",";
  j += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  j += "\"token\":\"" + jsonEscape(botToken) + "\",\"chatId\":\"" + jsonEscape(chatId) + "\",";
  j += "\"morningEnabled\":" + String(morningEnabled ? "true" : "false") + ",\"morningHour\":" + String(morningHour) + ",\"msgMorning\":\"" + jsonEscape(msgMorning) + "\",";
  j += "\"cloudEnabled\":" + String(fbEnabled ? "true" : "false") + ",\"deviceId\":\"" + jsonEscape(deviceId) + "\",";
  j += "\"history\":[";
  for (int i = 0; i < histCount; i++) { if (i) j += ","; j += String(histBuf[i]); }
  j += "],\"log\":[";
  for (int i = logN - 1, c = 0; i >= 0; i--, c++) {
    if (c) j += ",";
    j += "{\"level\":\"" + logLevel[i] + "\",\"message\":\"" + jsonEscape(logMsg[i]) + "\",\"time\":\"" + logTime[i] + "\"}";
  }
  j += "]}";
  return j;
}

void handleStatus() {
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void sendOk()            { server.send(200, "application/json", "{\"ok\":true}"); }
void sendErr(String m)   { server.send(200, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(m) + "\"}"); }

void handleThresholds() {
  int h = server.arg("high").toInt();
  int m = server.arg("med").toInt();
  int l = server.arg("low").toInt();
  int y = server.arg("hyst").toInt();
  if (l < 0 || h > 100 || !(l < m && m < h) || y < 0 || y > 20) { sendErr("ค่าไม่ถูกต้อง"); return; }
  highPct = h; medPct = m; lowPct = l; hystPct = y;
  saveConfig();
  // ประเมิน band ใหม่ตามเกณฑ์ล่าสุด (เงียบ ไม่สแปม)
  currentBand = decideBand(lastWaterPercent, 1);
  sendOk();
}

void handleMessages() {
  msgHigh   = server.arg("high");
  msgMedium = server.arg("med");
  msgLow    = server.arg("low");
  saveConfig();
  sendOk();
}

void handleTelegram() {
  botToken = server.arg("token");
  chatId   = server.arg("chatId");
  saveConfig();
  sendOk();
}

void handleTest() {
  bool ok = sendTelegramMessage("📩 ทดสอบส่งข้อความสำเร็จ");
  if (ok) addLog("wifi", "📩 ทดสอบส่งข้อความสำเร็จ");
  if (ok) sendOk(); else sendErr("ส่งไม่สำเร็จ");
}

void handleMorning() {
  morningEnabled = (server.arg("enabled") == "1");
  int hr = server.arg("hour").toInt();
  if (hr >= 0 && hr <= 23) morningHour = hr;
  msgMorning = server.arg("msg");
  saveConfig();
  sendOk();
}

void handleCalib() {
  int mn = server.arg("adcMin").toInt();
  int mx = server.arg("adcMax").toInt();
  if (mx <= mn) { sendErr("ADC 20mA ต้องมากกว่า 4mA"); return; }
  adcMin = mn; adcMax = mx;
  saveConfig();
  sendOk();
}

void handleForget() {
  sendOk();
  delay(300);
  WiFi.disconnect(true, true);
  delay(500);
  WiFiManager wm; wm.resetSettings();
  delay(500);
  ESP.restart();
}

// ================= FIREBASE (cloud, used by the mobile app from anywhere) =================
String fbStatusPath() { return "/devices/" + deviceId + "/status"; }
String fbCmdPath()    { return "/devices/" + deviceId + "/cmd"; }

void fbPublishStatus() {
  if (!fbInited || !Firebase.ready()) return;
  FirebaseJson json;
  json.setJsonData(buildStatusJson());          // reuse the same JSON as the HTTP API
  Firebase.RTDB.setJSON(&fbdo, fbStatusPath().c_str(), &json);
}

// Apply one command object (the app writes it to /devices/<id>/cmd)
void handleCloudCommand(FirebaseJson &json) {
  FirebaseJsonData r;
  String action;
  if (json.get(r, "action")) action = r.to<String>();
  if (action == "") return;

  if (action == "thresholds") {
    int h = highPct, m = medPct, l = lowPct, y = hystPct;
    if (json.get(r, "high")) h = r.to<int>();
    if (json.get(r, "med"))  m = r.to<int>();
    if (json.get(r, "low"))  l = r.to<int>();
    if (json.get(r, "hyst")) y = r.to<int>();
    if (l >= 0 && l < m && m < h && h <= 100 && y >= 0 && y <= 20) {
      highPct = h; medPct = m; lowPct = l; hystPct = y;
      saveConfig();
      currentBand = decideBand(lastWaterPercent, 1);   // re-evaluate quietly
    }
  } else if (action == "messages") {
    if (json.get(r, "high")) msgHigh   = r.to<String>();
    if (json.get(r, "med"))  msgMedium = r.to<String>();
    if (json.get(r, "low"))  msgLow    = r.to<String>();
    saveConfig();
  } else if (action == "telegram") {
    if (json.get(r, "token"))  botToken = r.to<String>();
    if (json.get(r, "chatId")) chatId   = r.to<String>();
    saveConfig();
  } else if (action == "test") {
    if (sendTelegramMessage("📩 ทดสอบส่งข้อความสำเร็จ")) addLog("wifi", "📩 ทดสอบส่งข้อความสำเร็จ");
  } else if (action == "morning") {
    if (json.get(r, "enabled")) morningEnabled = r.to<int>() != 0;
    if (json.get(r, "hour"))   { int hr = r.to<int>(); if (hr >= 0 && hr <= 23) morningHour = hr; }
    if (json.get(r, "msg"))     msgMorning = r.to<String>();
    saveConfig();
  } else if (action == "calib") {
    int mn = adcMin, mx = adcMax;
    if (json.get(r, "adcMin")) mn = r.to<int>();
    if (json.get(r, "adcMax")) mx = r.to<int>();
    if (mx > mn) { adcMin = mn; adcMax = mx; saveConfig(); }
  } else if (action == "forget") {
    Firebase.RTDB.deleteNode(&fbdo, fbCmdPath().c_str());
    delay(200);
    WiFi.disconnect(true, true); delay(500);
    WiFiManager wm; wm.resetSettings(); delay(500);
    ESP.restart();
  }
  fbPublishStatus();   // echo new state back to the app immediately
}

// Poll the command node; run it, then clear it so it isn't run twice
void fbPollCommands() {
  if (!fbInited || !Firebase.ready()) return;
  if (Firebase.RTDB.getJSON(&fbdo, fbCmdPath().c_str())) {
    if (fbdo.dataType() == "json") {
      FirebaseJson &json = fbdo.jsonObject();
      handleCloudCommand(json);
      Firebase.RTDB.deleteNode(&fbdo, fbCmdPath().c_str());
    }
  }
}

void setupFirebase() {
  if (!fbEnabled || fbDatabaseUrl == "") return;
  fbCfg.api_key      = fbApiKey.c_str();
  fbCfg.database_url = fbDatabaseUrl.c_str();
  fbCfg.token_status_callback = tokenStatusCallback;   // from addons/TokenHelper.h
  if (fbUserEmail.length()) {
    fbAuth.user.email    = fbUserEmail.c_str();
    fbAuth.user.password = fbUserPass.c_str();
  } else {
    Firebase.signUp(&fbCfg, &fbAuth, "", "");           // anonymous sign-in
  }
  Firebase.begin(&fbCfg, &fbAuth);
  Firebase.reconnectWiFi(true);
  fbdo.setBSSLBufferSize(4096, 1024);
  fbInited = true;
}

// LAN endpoint so the app (on same Wi-Fi) can configure Firebase on the device
void handleCloud() {
  fbEnabled = (server.arg("enabled") == "1");
  if (server.hasArg("apiKey"))   fbApiKey      = server.arg("apiKey");
  if (server.hasArg("dbUrl"))    fbDatabaseUrl = server.arg("dbUrl");
  if (server.hasArg("email"))    fbUserEmail   = server.arg("email");
  if (server.hasArg("pass"))     fbUserPass    = server.arg("pass");
  if (server.hasArg("deviceId")) deviceId      = server.arg("deviceId");
  saveConfig();
  sendOk();
  delay(200);
  ESP.restart();   // simplest reliable way to re-init Firebase with new creds
}

// ================= WEB PAGE (embedded) =================
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>ระบบตรวจสอบน้ำ</title>
<style>
:root{--bg:#F2F2F7;--card:#fff;--txt:#1C1C1E;--sub:#8E8E93;--blue:#0A84FF;--orange:#FF9500;--red:#FF3B30;--green:#22C55E;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{margin:0;font-family:-apple-system,system-ui,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--txt);}
#app{max-width:440px;margin:0 auto;min-height:100vh;padding-bottom:96px;}
.hdr{padding:16px 20px 10px;position:sticky;top:0;background:var(--bg);z-index:5;}
.title{font-size:28px;font-weight:700;letter-spacing:-.3px;}
.row-between{display:flex;align-items:center;justify-content:space-between;}
.badge{display:flex;align-items:center;gap:6px;padding:6px 10px;border-radius:20px;font-size:13px;font-weight:600;}
.dot{width:7px;height:7px;border-radius:50%;}
.sub{font-size:13px;color:var(--sub);}
.content{padding:0 20px;}
.card{background:var(--card);border-radius:18px;box-shadow:0 1px 3px rgba(0,0,0,.05);}
.pad{padding:16px;}
.mt16{margin-top:16px;}
.back{display:flex;align-items:center;gap:4px;color:var(--blue);font-size:17px;margin-bottom:6px;cursor:pointer;}
.tank-wrap{display:flex;gap:16px;}
.tankcard{padding:20px;display:flex;flex-direction:column;align-items:center;flex-shrink:0;}
.tank{position:relative;width:96px;height:170px;}
.tank .glass{position:absolute;inset:0;border:3px solid #D1D1D6;border-radius:14px;overflow:hidden;background:#FAFAFA;}
.fill{position:absolute;bottom:0;left:0;right:0;overflow:visible;transition:height .4s ease;}
.marker{position:absolute;left:-2px;right:-2px;height:1px;background:repeating-linear-gradient(90deg,#C7C7CC 0 4px,transparent 4px 8px);}
.big{font-size:30px;font-weight:700;margin-top:14px;}
.srow{display:flex;align-items:center;justify-content:space-between;padding:12px 14px;border-radius:16px;background:var(--card);box-shadow:0 1px 3px rgba(0,0,0,.04);}
.srow .lab{font-size:14px;font-weight:600;}
.srow .sm{font-size:11px;color:var(--sub);}
.sdot{width:12px;height:12px;border-radius:50%;}
.alarm{background:#FDEBEA;border:1px solid #F5B5B0;border-radius:14px;padding:12px 14px;display:flex;gap:10px;color:#B3261E;font-size:13px;line-height:1.4;}
.chip{padding:7px 16px;border-radius:20px;font-size:13px;font-weight:600;cursor:pointer;box-shadow:0 1px 2px rgba(0,0,0,.04);}
.tl-icon{width:30px;height:30px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:12px;flex-shrink:0;}
.setrow{display:flex;align-items:center;padding:14px 16px;border-bottom:.5px solid #E5E5EA;cursor:pointer;}
.setrow:last-child{border-bottom:none;}
.setico{width:30px;height:30px;border-radius:8px;display:flex;align-items:center;justify-content:center;margin-right:12px;color:#fff;font-size:15px;font-weight:700;}
.setrow .nm{flex:1;font-size:16px;}
.setrow .vv{font-size:15px;color:var(--sub);margin-right:6px;}
.chev{color:#C7C7CC;font-size:18px;}
.field{padding:12px 0;border-bottom:.5px solid #E5E5EA;}
.field:last-child{border-bottom:none;}
.field label{display:block;font-size:13px;color:var(--sub);margin-bottom:6px;}
input,textarea{width:100%;border:none;outline:none;font-size:16px;color:var(--txt);background:transparent;font-family:inherit;}
input[type=number]{font-family:inherit;}
textarea{resize:none;line-height:1.45;}
.numrow{display:flex;align-items:center;padding:14px 0;border-bottom:.5px solid #E5E5EA;}
.numrow:last-child{border-bottom:none;}
.numrow .nm{flex:1;font-size:16px;}
.numrow input{width:64px;text-align:right;font-size:17px;font-weight:600;}
.btn{border-radius:18px;padding:14px 16px;text-align:center;font-size:16px;font-weight:600;cursor:pointer;}
.btn-primary{background:var(--blue);color:#fff;}
.btn-white{background:#fff;color:var(--blue);box-shadow:0 1px 3px rgba(0,0,0,.04);}
.btn-danger{background:#fff;color:var(--red);box-shadow:0 1px 3px rgba(0,0,0,.04);}
.note{font-size:13px;color:var(--sub);padding:8px 4px 16px;line-height:1.5;}
.tabbar{position:fixed;bottom:0;left:0;right:0;max-width:440px;margin:0 auto;background:rgba(255,255,255,.92);backdrop-filter:blur(20px);border-top:.5px solid #D1D1D6;display:flex;padding:8px 0 22px;z-index:20;}
.tab{flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;cursor:pointer;font-size:10px;font-weight:600;}
.toast{position:fixed;bottom:110px;left:50%;transform:translateX(-50%);background:rgba(28,28,30,.92);color:#fff;font-size:14px;padding:10px 18px;border-radius:20px;z-index:40;opacity:0;transition:opacity .2s;pointer-events:none;}
.toast.show{opacity:1;}
.toggle{width:51px;height:31px;border-radius:16px;position:relative;cursor:pointer;transition:background .2s;}
.knob{position:absolute;top:2px;width:27px;height:27px;border-radius:50%;background:#fff;box-shadow:0 2px 4px rgba(0,0,0,.2);transition:left .2s;}
.step{width:32px;height:32px;border-radius:8px;background:#F2F2F7;display:flex;align-items:center;justify-content:center;cursor:pointer;font-size:18px;color:var(--blue);}
@keyframes wl-wave{from{transform:translateX(0);}to{transform:translateX(-96px);}}
@keyframes wl-wave2{from{transform:translateX(-96px);}to{transform:translateX(0);}}
@keyframes wl-bob{0%,100%{transform:translateY(0);}50%{transform:translateY(2.5px);}}
@keyframes wl-bubble{0%{transform:translateY(0) scale(.6);opacity:0;}15%{opacity:.7;}100%{transform:translateY(-70px) scale(1);opacity:0;}}
.bub{position:absolute;border-radius:50%;background:rgba(255,255,255,.55);}
</style></head>
<body>
<div id="app"></div>
<div id="toast" class="toast"></div>
<script>
var D=null, curScreen='dashboard', filter='all';
function $(id){return document.getElementById(id);}
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}

async function poll(){
  try{
    var r=await fetch('/api/status'); D=await r.json();
    if(curScreen==='dashboard'||curScreen==='history') render();
  }catch(e){}
}
function go(s){curScreen=s;render();window.scrollTo(0,0);}

function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(window._tt);window._tt=setTimeout(function(){t.classList.remove('show');},1600);}
async function postForm(url,obj){
  var body=new URLSearchParams(obj).toString();
  var r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
  return r.json();
}

function derive(){
  var band=D.band, p=Math.round(D.percent);
  var color='#0A84FF',label='ปกติ';
  if(band===0){color='#FF3B30';label='น้ำน้อย';}
  else if(band===3){color='#22C55E';label='เต็ม';}
  else if(band===2){color='#FF9500';label='ระดับกลาง';}
  var low=band===0;
  return {band:band,p:p,color:color,label:label,low:low,
    top:low?'#FF6961':'#5AC8FA',bot:low?'#FF3B30':'#0A84FF'};
}

function wave(){
  return '<div style="position:absolute;top:-8px;left:0;right:0;height:14px;animation:wl-bob 3s ease-in-out infinite;">'
   +'<svg width="192" height="20" viewBox="0 0 192 20" preserveAspectRatio="none" style="position:absolute;top:0;left:0;animation:wl-wave2 4.2s linear infinite;opacity:.28"><path d="M0 8 Q12 2 24 8 T48 8 T72 8 T96 8 T120 8 T144 8 T168 8 T192 8 V20 H0 Z" fill="#fff"></path></svg>'
   +'<svg width="192" height="20" viewBox="0 0 192 20" preserveAspectRatio="none" style="position:absolute;top:1px;left:0;animation:wl-wave 2.6s linear infinite;opacity:.45"><path d="M0 8 Q12 14 24 8 T48 8 T72 8 T96 8 T120 8 T144 8 T168 8 T192 8 V20 H0 Z" fill="#fff"></path></svg></div>'
   +'<div class="bub" style="bottom:8px;left:22px;width:5px;height:5px;animation:wl-bubble 3.4s ease-in infinite"></div>'
   +'<div class="bub" style="bottom:4px;left:52px;width:3px;height:3px;animation:wl-bubble 2.8s ease-in infinite 1.1s"></div>'
   +'<div class="bub" style="bottom:6px;left:68px;width:4px;height:4px;animation:wl-bubble 3.9s ease-in infinite .6s"></div>';
}

function chartSVG(){
  var hist=D.history&&D.history.length?D.history:[Math.round(D.percent)];
  var W=320,H=150,padT=12,padB=12,plotH=H-padT-padB,n=hist.length;
  function xAt(i){return n<=1?0:(i/(n-1))*W;}
  function yAt(v){return padT+(1-v/100)*plotH;}
  var pts=hist.map(function(v,i){return xAt(i).toFixed(1)+','+yAt(v).toFixed(1);}).join(' ');
  var area='M0,'+H+' '+hist.map(function(v,i){return 'L'+xAt(i).toFixed(1)+','+yAt(v).toFixed(1);}).join(' ')+' L'+W+','+H+' Z';
  var d=derive();
  var fill=d.low?'rgba(255,59,48,.12)':'rgba(10,132,255,.12)';
  var lx=xAt(n-1).toFixed(1),ly=yAt(hist[n-1]).toFixed(1);
  function line(v,c){return '<line x1="0" y1="'+yAt(v).toFixed(1)+'" x2="320" y2="'+yAt(v).toFixed(1)+'" stroke="'+c+'" stroke-width="1" stroke-dasharray="3 4" opacity=".4" vector-effect="non-scaling-stroke"></line>';}
  return '<svg width="100%" height="150" viewBox="0 0 320 150" preserveAspectRatio="none" style="display:block;overflow:visible">'
    +line(D.high,'#0A84FF')+line(D.med,'#FF9500')+line(D.low,'#FF3B30')
    +'<path d="'+area+'" fill="'+fill+'"></path>'
    +'<polyline points="'+pts+'" fill="none" stroke="'+d.color+'" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round" vector-effect="non-scaling-stroke"></polyline>'
    +'<circle cx="'+lx+'" cy="'+ly+'" r="4" fill="'+d.color+'" stroke="#fff" stroke-width="2"></circle></svg>';
}

function header(title,showBack,badge){
  var h='<div class="hdr">';
  if(showBack) h+='<div class="back" onclick="go(\'settings\')">‹ ตั้งค่า</div>';
  h+='<div class="row-between"><div class="title">'+esc(title)+'</div>';
  if(badge){
    var on=D.wifi;
    h+='<div class="badge" style="background:'+(on?'#E7F8EE':'#FDEBEA')+';color:'+(on?'#1C8A4C':'#B3261E')+'"><span class="dot" style="background:'+(on?'#22C55E':'#FF3B30')+'"></span>'+(on?'ออนไลน์':'ออฟไลน์')+'</div>';
  }
  h+='</div>';
  if(badge) h+='<div class="sub" style="margin-top:2px">อัปเดตล่าสุด '+esc(D.lastUpdate)+'</div>';
  h+='</div>';
  return h;
}

function tabbar(){
  function c(s){return curScreen===s?'#0A84FF':'#8E8E93';}
  return '<div class="tabbar">'
   +'<div class="tab" style="color:'+c('dashboard')+'" onclick="go(\'dashboard\')">🏠<span>หน้าหลัก</span></div>'
   +'<div class="tab" style="color:'+c('history')+'" onclick="go(\'history\')">🕘<span>ประวัติ</span></div>'
   +'<div class="tab" style="color:'+c('settings')+'" onclick="go(\'settings\')">⚙️<span>ตั้งค่า</span></div>'
   +'</div>';
}

function viewDashboard(){
  var d=derive();
  var srow=function(lab,active,col,txtOn){
    return '<div class="srow"><div><div class="lab">'+lab+'</div><div class="sm">'+(active?txtOn:'ปกติ')+'</div></div>'
      +'<div class="sdot" style="background:'+(active?col:'#D1D1D6')+';box-shadow:0 0 0 4px '+(active?col+'22':'transparent')+'"></div></div>';
  };
  var h=header('ระบบตรวจสอบน้ำ',false,true);
  h+='<div class="content"><div class="tank-wrap">';
  // tank
  h+='<div class="card tankcard"><div class="tank"><div class="glass">'
    +'<div class="fill" style="height:'+d.p+'%;background:linear-gradient(180deg,'+d.top+','+d.bot+')">'+wave()+'</div></div>'
    +'<div class="marker" style="top:'+(100-D.high)+'%"></div>'
    +'<div class="marker" style="top:'+(100-D.med)+'%"></div>'
    +'<div class="marker" style="top:'+(100-D.low)+'%"></div></div>'
    +'<div class="big">'+d.p+'%</div><div style="font-size:13px;font-weight:600;color:'+d.color+';margin-top:2px">'+d.label+'</div></div>';
  // rows
  h+='<div style="flex:1;display:flex;flex-direction:column;gap:10px">'
    +srow('High '+D.high+'%',d.band===3,'#0A84FF','ทำงานอยู่')
    +srow('Medium '+D.med+'%',d.band===2,'#FF9500','ทำงานอยู่')
    +srow('Low '+D.low+'%',d.band===0,'#FF3B30','แจ้งเตือน!');
  if(d.low) h+='<div class="alarm"><span>⚠️</span><div>'+esc(D.msgLow)+'</div></div>';
  h+='</div></div>';
  // graph
  h+='<div class="card pad mt16"><div class="row-between" style="margin-bottom:12px"><div class="lab" style="font-size:14px;font-weight:600">แนวโน้มระดับน้ำ</div><div class="sub" style="font-size:12px">ย้อนหลัง ~2 นาที</div></div>'
    +'<div style="display:flex"><div style="width:26px;height:150px;display:flex;flex-direction:column;justify-content:space-between;align-items:flex-end;padding:7px 0;flex-shrink:0;font-size:10px;color:#B0B0B5"><span>100</span><span>75</span><span>50</span><span>25</span><span>0</span></div>'
    +'<div style="flex:1;height:150px">'+chartSVG()+'</div></div>'
    +'<div style="display:flex;gap:16px;margin-top:12px;padding-top:12px;border-top:.5px solid #F0F0F0;font-size:11px;color:#8E8E93">'
    +'<span><span style="display:inline-block;width:12px;height:2px;background:#0A84FF;vertical-align:middle"></span> สูง '+D.high+'%</span>'
    +'<span><span style="display:inline-block;width:12px;height:2px;background:#FF9500;vertical-align:middle"></span> กลาง '+D.med+'%</span>'
    +'<span><span style="display:inline-block;width:12px;height:2px;background:#FF3B30;vertical-align:middle"></span> ต่ำ '+D.low+'%</span></div></div>';
  h+='</div>';
  return h;
}

function viewHistory(){
  var meta={high:{l:'ระดับสูง',c:'#0A84FF',i:'▲'},medium:{l:'ระดับกลาง',c:'#FF9500',i:'●'},low:{l:'ระดับต่ำ',c:'#FF3B30',i:'▼'},wifi:{l:'ระบบ',c:'#34C759',i:'✓'}};
  var isAlert=function(lv){return lv==='high'||lv==='medium'||lv==='low';};
  var rows=(D.log||[]).filter(function(r){return filter==='all'?true:filter==='alert'?isAlert(r.level):!isAlert(r.level);});
  function chip(k,lab){var on=filter===k;return '<div class="chip" style="background:'+(on?'#1C1C1E':'#fff')+';color:'+(on?'#fff':'#8E8E93')+'" onclick="setFilter(\''+k+'\')">'+lab+'</div>';}
  var h=header('ประวัติการแจ้งเตือน',false,false);
  h+='<div class="content"><div style="display:flex;gap:8px;margin-bottom:18px">'+chip('all','ทั้งหมด')+chip('alert','แจ้งเตือน')+chip('system','ระบบ')+'</div>';
  if(rows.length===0){
    h+='<div style="text-align:center;padding:60px 20px;color:#8E8E93"><div style="font-size:32px;margin-bottom:8px">📋</div><div>ไม่มีรายการในหมวดนี้</div></div>';
  }else{
    h+='<div class="row-between" style="padding:0 4px;margin-bottom:10px"><div class="sub" style="font-weight:600;text-transform:uppercase;letter-spacing:.3px">วันนี้</div><div class="sub" style="font-size:12px">'+rows.length+' รายการ</div></div>';
    h+='<div class="card" style="padding:16px 16px 4px">';
    rows.forEach(function(r,i){
      var m=meta[r.level]||meta.wifi, last=i===rows.length-1;
      h+='<div style="display:flex;gap:12px"><div style="display:flex;flex-direction:column;align-items:center">'
        +'<div class="tl-icon" style="background:'+m.c+'1A;color:'+m.c+'">'+m.i+'</div>'
        +'<div style="flex:1;width:2px;background:'+(last?'transparent':'#E5E5EA')+';margin:3px 0"></div></div>'
        +'<div style="flex:1;min-width:0;padding-bottom:16px"><div class="row-between"><span style="font-size:13px;font-weight:700;color:'+m.c+'">'+m.l+'</span><span class="sub" style="font-size:12px">'+esc(r.time)+'</span></div>'
        +'<div style="font-size:13px;color:#3A3A3C;margin-top:3px;line-height:1.45">'+esc(r.message)+'</div></div></div>';
    });
    h+='</div>';
  }
  h+='</div>';
  return h;
}
function setFilter(k){filter=k;render();}

function viewSettings(){
  var h=header('ตั้งค่า',false,false);
  h+='<div class="content"><div class="card">';
  function row(ico,bg,name,val,scr){
    return '<div class="setrow" onclick="go(\''+scr+'\')"><div class="setico" style="background:'+bg+'">'+ico+'</div><div class="nm">'+name+'</div>'+(val?'<div class="vv">'+esc(val)+'</div>':'')+'<div class="chev">›</div></div>';
  }
  h+=row('📶','#0A84FF','Wi-Fi',D.ssid||'-','wifi');
  h+=row('✈️','#229ED9','Telegram','','telegram');
  h+=row('☰','#FF9500','เกณฑ์แจ้งเตือน',D.low+'/'+D.med+'/'+D.high+'%','thresholds');
  h+=row('💬','#34C759','ข้อความแจ้งเตือน','','messages');
  h+=row('🌅','#FFCC00','รายงานเช้า',String(D.morningHour).padStart(2,'0')+':00','morning');
  h+=row('📈','#5856D6','ตั้งค่าเซนเซอร์ ADC','4–20mA','calib');
  h+='</div></div>';
  return h;
}

function viewWifi(){
  var h=header('Wi-Fi',true,false)+'<div class="content">';
  h+='<div class="card pad" style="margin-bottom:16px">'
    +'<div class="field"><div class="row-between"><span class="sub">สถานะ</span><span style="font-size:15px;font-weight:600;color:'+(D.wifi?'#22C55E':'#FF3B30')+'">'+(D.wifi?'เชื่อมต่อแล้ว':'ไม่ได้เชื่อมต่อ')+'</span></div></div>'
    +'<div class="field"><label>ชื่อเครือข่าย (SSID)</label><div style="font-size:16px">'+esc(D.ssid||'-')+'</div></div></div>';
  h+='<div class="note">การเชื่อมต่อ Wi-Fi ตั้งค่าผ่าน Captive Portal ของ WiFiManager หากต้องการเปลี่ยนเครือข่าย กดปุ่มด้านล่างเพื่อรีเซ็ต แล้วอุปกรณ์จะเปิด AP ชื่อ "WaterLevel_Setup" ให้ตั้งค่าใหม่</div>';
  h+='<div class="btn btn-danger" onclick="forgetWifi()">ลืมเครือข่ายนี้ / รีเซ็ต Wi-Fi</div>';
  h+='</div>';
  return h;
}
async function forgetWifi(){
  if(!confirm('รีเซ็ต Wi-Fi และรีสตาร์ทอุปกรณ์?'))return;
  await postForm('/api/forget',{});
  toast('กำลังรีสตาร์ท...');
}

function viewTelegram(){
  var h=header('Telegram',true,false)+'<div class="content">';
  h+='<div class="card pad" style="margin-bottom:16px">'
    +'<div class="field"><label>Bot Token</label><input id="f_token" value="'+esc(D.token)+'" style="font-family:monospace;font-size:15px"></div>'
    +'<div class="field"><label>Chat ID</label><input id="f_chat" value="'+esc(D.chatId)+'" style="font-family:monospace"></div></div>';
  h+='<div class="btn btn-white" style="color:#229ED9;margin-bottom:12px" onclick="testTelegram()">ส่งข้อความทดสอบ</div>';
  h+='<div class="btn btn-primary" onclick="saveTelegram()">บันทึก</div></div>';
  return h;
}
async function saveTelegram(){
  var r=await postForm('/api/telegram',{token:$('f_token').value,chatId:$('f_chat').value});
  if(r.ok){D.token=$('f_token').value;D.chatId=$('f_chat').value;toast('บันทึกแล้ว ✓');}else toast(r.error||'ผิดพลาด');
}
async function testTelegram(){
  toast('กำลังส่ง...');
  var r=await postForm('/api/test',{});
  toast(r.ok?'ส่งข้อความทดสอบสำเร็จ ✓':(r.error||'ส่งไม่สำเร็จ'));
}

function viewThresholds(){
  var h=header('เกณฑ์แจ้งเตือน',true,false)+'<div class="content">';
  function nr(dot,name,id,val){return '<div class="numrow"><div class="'+(dot?'sdot':'')+'" style="'+(dot?'width:10px;height:10px;background:'+dot+';margin-right:10px':'')+'"></div><div class="nm">'+name+'</div><input type="number" id="'+id+'" value="'+val+'"><span class="sub" style="margin-left:2px">%</span></div>';}
  h+='<div class="card pad" style="margin-bottom:12px">'
    +nr('#0A84FF','ระดับสูง (High)','t_high',D.high)
    +nr('#FF9500','ระดับกลาง (Medium)','t_med',D.med)
    +nr('#FF3B30','ระดับต่ำ (Low)','t_low',D.low)+'</div>';
  h+='<div class="card pad" style="margin-bottom:12px"><div class="numrow"><div class="nm">ระยะกันสั่น (Hysteresis)<div class="sub" style="font-size:12px;margin-top:2px">กันแจ้งเตือนซ้ำเมื่อค่าแกว่งที่ขอบเกณฑ์</div></div><input type="number" id="t_hyst" value="'+D.hyst+'"><span class="sub" style="margin-left:2px">%</span></div></div>';
  h+='<div class="note">เงื่อนไข: ต่ำ &lt; กลาง &lt; สูง (0–100%) ระบบจะแจ้งเตือนเฉพาะเมื่อ<b>เปลี่ยนระดับ</b>เท่านั้น</div>';
  h+='<div class="btn btn-primary" onclick="saveThresholds()">บันทึก</div></div>';
  return h;
}
async function saveThresholds(){
  var o={high:$('t_high').value,med:$('t_med').value,low:$('t_low').value,hyst:$('t_hyst').value};
  var r=await postForm('/api/thresholds',o);
  if(r.ok){D.high=+o.high;D.med=+o.med;D.low=+o.low;D.hyst=+o.hyst;toast('บันทึกเกณฑ์แล้ว ✓');}else toast(r.error||'ค่าไม่ถูกต้อง');
}

function viewMessages(){
  var h=header('ข้อความแจ้งเตือน',true,false)+'<div class="content">';
  h+='<div class="note">ตั้งข้อความที่จะส่งเมื่อระดับน้ำเข้าสู่แต่ละช่วง ระบบจะต่อท้ายด้วยค่าเปอร์เซ็นต์ปัจจุบันอัตโนมัติ</div>';
  function box(col,title,id,val){return '<div class="card pad" style="margin-bottom:14px"><div style="display:flex;align-items:center;gap:8px;margin-bottom:8px"><div style="width:10px;height:10px;border-radius:50%;background:'+col+'"></div><div style="font-size:13px;font-weight:700;color:'+col+'">'+title+'</div></div><textarea id="'+id+'" rows="2">'+esc(val)+'</textarea></div>';}
  h+=box('#0A84FF','ระดับสูง (≥ '+D.high+'%)','m_high',D.msgHigh);
  h+=box('#FF9500','ระดับกลาง (≥ '+D.med+'%)','m_med',D.msgMedium);
  h+=box('#FF3B30','ระดับต่ำ (< '+D.low+'%)','m_low',D.msgLow);
  h+='<div class="btn btn-primary mt16" onclick="saveMessages()">บันทึก</div></div>';
  return h;
}
async function saveMessages(){
  var o={high:$('m_high').value,med:$('m_med').value,low:$('m_low').value};
  var r=await postForm('/api/messages',o);
  if(r.ok){D.msgHigh=o.high;D.msgMedium=o.med;D.msgLow=o.low;toast('บันทึกแล้ว ✓');}else toast('ผิดพลาด');
}

var mMorningH=null;
function viewMorning(){
  if(mMorningH===null) mMorningH=D.morningHour;
  var h=header('รายงานเช้า',true,false)+'<div class="content">';
  h+='<div class="card pad" style="margin-bottom:16px"><div class="row-between" style="padding:2px 0"><span style="font-size:16px">เปิดใช้งานรายงานเช้า</span>'
    +'<div class="toggle" id="m_tog" style="background:'+(D.morningEnabled?'#34C759':'#E5E5EA')+'" onclick="toggleMorning()"><div class="knob" style="left:'+(D.morningEnabled?22:2)+'px"></div></div></div></div>';
  h+='<div class="card pad" style="margin-bottom:16px"><div class="row-between" style="padding:2px 0"><span style="font-size:16px">เวลาแจ้งเตือน</span>'
    +'<div style="display:flex;align-items:center;gap:10px"><div class="step" onclick="hourStep(-1)">−</div><span id="m_hr" style="font-size:18px;font-weight:600;min-width:66px;text-align:center">'+String(mMorningH).padStart(2,'0')+':00</span><div class="step" onclick="hourStep(1)">+</div></div></div></div>';
  h+='<div class="card pad" style="margin-bottom:12px"><div style="font-size:13px;font-weight:600;color:#FFB100;margin-bottom:8px">ข้อความรายงานเช้า</div><textarea id="m_msg" rows="2">'+esc(D.msgMorning)+'</textarea></div>';
  h+='<div style="background:#FFF8E8;border-radius:14px;padding:12px 14px;margin-bottom:16px"><div style="font-size:11px;color:#A8791A;margin-bottom:4px">ตัวอย่างข้อความที่จะส่ง</div><div id="m_prev" style="font-size:13px;color:#6B5210;line-height:1.45">'+esc('☀️ รายงานสถานะ '+String(mMorningH).padStart(2,'0')+':00 น. '+D.msgMorning)+'</div></div>';
  h+='<div class="btn btn-primary" onclick="saveMorning()">บันทึก</div></div>';
  return h;
}
function toggleMorning(){D.morningEnabled=!D.morningEnabled;var t=$('m_tog');t.style.background=D.morningEnabled?'#34C759':'#E5E5EA';t.firstChild.style.left=(D.morningEnabled?22:2)+'px';}
function hourStep(dir){mMorningH=(mMorningH+dir+24)%24;$('m_hr').textContent=String(mMorningH).padStart(2,'0')+':00';$('m_prev').textContent='☀️ รายงานสถานะ '+String(mMorningH).padStart(2,'0')+':00 น. '+$('m_msg').value;}
async function saveMorning(){
  var o={enabled:D.morningEnabled?1:0,hour:mMorningH,msg:$('m_msg').value};
  var r=await postForm('/api/morning',o);
  if(r.ok){D.morningHour=mMorningH;D.msgMorning=o.msg;toast('บันทึกแล้ว ✓');}else toast('ผิดพลาด');
}

function viewCalib(){
  var h=header('ตั้งค่าเซนเซอร์ ADC',true,false)+'<div class="content">';
  h+='<div style="background:#EAF0FF;border-radius:14px;padding:14px 16px;margin-bottom:16px" class="row-between">'
    +'<div><div style="font-size:12px;color:#5A6B8C">ค่าที่อ่านได้ปัจจุบัน</div><div style="font-size:22px;font-weight:700;color:#0A84FF">'+D.adc+' <span style="font-size:13px;color:#8E8E93">ADC</span></div></div>'
    +'<div style="text-align:right"><div style="font-size:12px;color:#5A6B8C">กระแส</div><div style="font-size:22px;font-weight:700;color:#0A84FF">'+D.ma+' <span style="font-size:13px;color:#8E8E93">mA</span></div></div></div>';
  h+='<div class="card pad" style="margin-bottom:12px"><div class="field"><label>ADC ที่ 4mA (0%)</label><input type="number" id="c_min" value="'+D.adcMin+'" style="font-family:monospace"></div>'
    +'<div class="field"><label>ADC ที่ 20mA (100%)</label><input type="number" id="c_max" value="'+D.adcMax+'" style="font-family:monospace"></div></div>';
  h+='<div class="note">ปรับค่า ADC ให้ตรงกับสัญญาณ 4–20mA หน้างานจริง ระบบเฉลี่ยค่า 16 ครั้งเพื่อกรองสัญญาณรบกวน และใช้ hysteresis กันการแจ้งเตือนซ้ำ</div>';
  h+='<div class="btn btn-primary" onclick="saveCalib()">บันทึก</div></div>';
  return h;
}
async function saveCalib(){
  var o={adcMin:$('c_min').value,adcMax:$('c_max').value};
  var r=await postForm('/api/calib',o);
  if(r.ok){D.adcMin=+o.adcMin;D.adcMax=+o.adcMax;toast('บันทึกแล้ว ✓');}else toast(r.error||'ผิดพลาด');
}

function render(){
  if(!D){$('app').innerHTML='<div style="padding:80px 20px;text-align:center;color:#8E8E93">กำลังโหลด...</div>';return;}
  if(curScreen!=='morning') mMorningH=null;
  var body;
  switch(curScreen){
    case 'history': body=viewHistory(); break;
    case 'settings': body=viewSettings(); break;
    case 'wifi': body=viewWifi(); break;
    case 'telegram': body=viewTelegram(); break;
    case 'thresholds': body=viewThresholds(); break;
    case 'messages': body=viewMessages(); break;
    case 'morning': body=viewMorning(); break;
    case 'calib': body=viewCalib(); break;
    default: body=viewDashboard();
  }
  var showTab=['dashboard','history','settings'].indexOf(curScreen)>=0;
  $('app').innerHTML=body+(showTab?tabbar():'');
}

window.go=go;window.setFilter=setFilter;window.forgetWifi=forgetWifi;
window.saveTelegram=saveTelegram;window.testTelegram=testTelegram;window.saveThresholds=saveThresholds;
window.saveMessages=saveMessages;window.toggleMorning=toggleMorning;window.hourStep=hourStep;window.saveMorning=saveMorning;window.saveCalib=saveCalib;

render();
poll();
setInterval(poll,2000);
</script>
</body></html>)HTMLPAGE";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void setupWebServer() {
  // Allow the standalone PWA (served from another origin, e.g. a phone
  // home-screen app or localhost) to call the API cross-origin.
  server.enableCORS(true);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/thresholds", HTTP_POST, handleThresholds);
  server.on("/api/messages", HTTP_POST, handleMessages);
  server.on("/api/telegram", HTTP_POST, handleTelegram);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/morning", HTTP_POST, handleMorning);
  server.on("/api/calib", HTTP_POST, handleCalib);
  server.on("/api/forget", HTTP_POST, handleForget);
  server.on("/api/cloud", HTTP_POST, handleCloud);
  server.begin();
}

// ================= MORNING REPORT =================
void checkMorningOnline() {
  if (!WiFi.isConnected() || !morningEnabled) return;
  struct tm t;
  if (!getLocalTime(&t)) return;
  if (t.tm_hour == morningHour && t.tm_min == 0 && !morningNotifySent) {
    char hh[6]; sprintf(hh, "%02d:00", morningHour);
    String m = "☀️ รายงานสถานะ " + String(hh) + " น. " + msgMorning;
    sendTelegramMessage(m);
    addLog("wifi", m);
    morningNotifySent = true;
  }
  if (t.tm_hour != morningHour) morningNotifySent = false;
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
      WiFiManager wm; wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

// ================= WIFI =================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("WaterLevel_Setup")) ESP.restart();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  wifiWasConnected = true;

  String ip = WiFi.localIP().toString();
  Serial.println("Web app: http://" + ip);
  sendTelegramMessage("✅ ระบบ Online\nเปิดแอปที่ http://" + ip);
  addLog("wifi", "✅ ระบบ Online");
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);
  analogReadResolution(12);
  pinMode(pinLedWifi, OUTPUT);
  pinMode(pinResetWifi, INPUT_PULLUP);

  loadConfig();
  startWiFi();
  setupWebServer();
  setupFirebase();
}

// ================= LOOP =================
void loop() {
  digitalWrite(pinLedWifi, WiFi.isConnected());
  server.handleClient();

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
      addLog("wifi", "✅ WiFi กลับมาแล้ว");
    }
  }

  // อ่านระดับน้ำทุก 1 วินาที
  if (millis() - lastWaterCheck > 1000) {
    lastWaterCheck = millis();
    checkWaterLevelAnalog();
  }

  // เก็บกราฟย้อนหลังทุก 2.5 วินาที (48 จุด ≈ 2 นาที)
  if (bandInitialized && millis() - lastHistSample > 2500) {
    lastHistSample = millis();
    pushHistory((int)round(lastWaterPercent));
  }

  // Firebase: push status ~ทุก 3 วินาที และเช็คคำสั่งจากแอป ~ทุก 1.5 วินาที
  if (fbEnabled && fbInited && Firebase.ready()) {
    if (millis() - lastFbPublish > 3000) {
      lastFbPublish = millis();
      fbPublishStatus();
    }
    if (millis() - lastFbCmdPoll > 1500) {
      lastFbCmdPoll = millis();
      fbPollCommands();
    }
  }

  checkMorningOnline();
  checkResetButton();
}

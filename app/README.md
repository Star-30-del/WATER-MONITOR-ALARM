# ระบบตรวจสอบน้ำ — Water Monitor App

แอปมือถือ (iOS + Android) สำหรับดูระดับน้ำแบบเรียลไทม์จากอุปกรณ์ **ESP32**
UI ทั้งหมดอยู่ในโฟลเดอร์ `app/` (HTML/CSS/JS ไฟล์เดียว) ใช้เป็น **เว็บแอป/PWA** หรือห่อเป็นแอป native ด้วย Capacitor

## 3 โหมดการเชื่อมต่อ (เลือกที่หน้า ตั้งค่า → อุปกรณ์)

| โหมด | ใช้เมื่อ | กลไก |
|------|---------|------|
| **คลาวด์ (Cloud)** | อยู่ที่ไหนก็ได้ที่มีเน็ต | **Firebase Realtime Database** (realtime sync) |
| **Wi-Fi บ้าน (LAN)** | อยู่บ้าน วงเดียวกับอุปกรณ์ | เรียก REST API ของ ESP32 ตรง ๆ (`/api/status`, `/api/*`) |
| **ทดลอง (Demo)** | ไม่มีอุปกรณ์ | จำลองข้อมูลในเครื่อง (สไลเดอร์ + ปุ่มเล่น) |

โหมดที่เลือก + ค่าที่กรอกถูกจำไว้ใน `localStorage` เปิดครั้งต่อไปเชื่อมต่อให้อัตโนมัติ

---

## ตั้งค่า Firebase (ใช้ได้ทุกที่)

### 1) สร้างโปรเจกต์ + Realtime Database
1. ไป [Firebase Console](https://console.firebase.google.com/) → สร้างโปรเจกต์ (ฟรี Spark plan)
2. เมนู **Build → Realtime Database → Create Database** เลือก location แล้วเริ่มแบบ **Test mode** (หรือ Locked แล้วตั้ง rules ตามด้านล่าง)
3. **Project settings (⚙️) → General → Your apps → Web app (</>)**  → คัดลอก object `firebaseConfig`

### 2) กฎความปลอดภัย (Realtime Database → Rules)
เริ่มต้นแบบง่าย (จำกัดเฉพาะ path ของอุปกรณ์):
```json
{
  "rules": {
    "devices": {
      "$id": {
        ".read": true,
        ".write": true
      }
    }
  }
}
```
> ใช้งานจริงควรเปิด Authentication แล้วเปลี่ยนเป็น `"auth != null"` และให้ ESP32 ล็อกอินด้วย
> Email/Password (ตั้งใน `.ino`) — ดูหมายเหตุความปลอดภัยด้านล่าง

### 3) ฝั่งแอป
หน้า **ตั้งค่า → อุปกรณ์ → แท็บ “คลาวด์”**
- วาง object `firebaseConfig` ทั้งก้อนลงช่อง (วางแบบ `const firebaseConfig = {…}` ได้เลย แอปดึง `{…}` ให้เอง)
- ตั้ง **Device ID** (เช่น `wateralarm-1`) — **ต้องตรงกับ ESP32**
- กด “เชื่อมต่อ Firebase”

### 4) ฝั่ง ESP32
เปิด [`../WATER_ALARM.ino`](../WATER_ALARM.ino) แก้ค่าด้านบนไฟล์:
```cpp
bool   fbEnabled     = true;
String fbApiKey      = "AIza…";                     // จาก firebaseConfig.apiKey
String fbDatabaseUrl = "https://xxxx-default-rtdb.firebasedatabase.app"; // firebaseConfig.databaseURL
String fbUserEmail   = "";                           // ถ้าใช้ Email/Password auth
String fbUserPass    = "";
String deviceId      = "wateralarm-1";               // ต้องตรงกับแอป
```
ติดตั้ง library ผ่าน Arduino Library Manager: **"Firebase Arduino Client Library for ESP8266 and ESP32"** (โดย Mobizt)

> โครงข้อมูลใน RTDB:
> - `devices/<deviceId>/status` — ESP32 เขียนสถานะ (แอปอ่าน realtime)
> - `devices/<deviceId>/cmd` — แอปเขียนคำสั่ง (ESP32 อ่าน แล้วลบทิ้ง)

---

## รันเว็บแอปเพื่อทดสอบ

Service worker/PWA ต้องเสิร์ฟผ่าน HTTP ไม่ใช่ `file://`
```bash
python -m http.server 8777 --directory app
# เปิด http://localhost:8777/
```

## Build เป็นแอป native (ทางเลือก — Capacitor)

```bash
npm install
npx cap sync            # ทุกครั้งที่แก้ไฟล์ใน app/
npx cap open android    # ต้องมี Android Studio → Run/Build APK
# iOS: npx cap add ios แล้ว npx cap open ios (ต้องใช้ Mac + Xcode)
```

---

## หมายเหตุ

- **Demo/Cloud ใช้ได้ทุกที่** ส่วน **LAN** ต้องอยู่ Wi-Fi วงเดียวกับ ESP32
  (และถ้าเปิดแอปผ่าน HTTPS จะต่อ LAN ที่เป็น HTTP ไม่ได้ — mixed content; ใช้ Cloud แทน)
- Firebase SDK โหลดจาก CDN (gstatic) → โหมดคลาวด์ต้องมีเน็ต; โหมด Demo/LAN ทำงานออฟไลน์ได้
- สถานะที่ push ขึ้น RTDB มี bot token/chat id ด้วย — ถ้าเป็นข้อมูลจริงควรเปิด Auth + ตั้ง rules ให้ `auth != null`
- ESP32 ยังเสิร์ฟหน้าเว็บในตัวที่ `http://<ip>/` และ REST API เดิมได้เหมือนเดิม

## แผนที่คำสั่ง (แอป → อุปกรณ์)

| การทำงาน | LAN (HTTP POST) | Cloud (เขียน object ลง `devices/<id>/cmd`) |
|----------|-----------------|---------------------------------------------|
| เกณฑ์ | `/api/thresholds` | `{action:"thresholds",high,med,low,hyst}` |
| ข้อความ | `/api/messages` | `{action:"messages",high,med,low}` |
| Telegram | `/api/telegram` | `{action:"telegram",token,chatId}` |
| ทดสอบส่ง | `/api/test` | `{action:"test"}` |
| รายงานเช้า | `/api/morning` | `{action:"morning",enabled,hour,msg}` |
| คาลิเบรต ADC | `/api/calib` | `{action:"calib",adcMin,adcMax}` |
| รีเซ็ต Wi-Fi | `/api/forget` | `{action:"forget"}` |
| ตั้งค่า Firebase | `/api/cloud` | — (ตั้งในแอป/ในสเก็ตช์) |

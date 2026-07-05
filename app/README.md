# ระบบตรวจสอบน้ำ — Water Monitor App

แอปมือถือ (iOS + Android) สำหรับดูระดับน้ำแบบเรียลไทม์จากอุปกรณ์ **ESP32** ในโปรเจกต์นี้
UI ทั้งหมดอยู่ในโฟลเดอร์ `app/` (HTML/CSS/JS ไฟล์เดียว) แล้วห่อเป็นแอป native ด้วย **Capacitor**

## 3 โหมดการเชื่อมต่อ (เลือกที่หน้า ตั้งค่า → อุปกรณ์)

| โหมด | ใช้เมื่อ | กลไก |
|------|---------|------|
| **คลาวด์ (Cloud)** | อยู่ที่ไหนก็ได้ที่มีเน็ต | MQTT over WebSocket ผ่าน cloud broker (เช่น HiveMQ Cloud) |
| **Wi-Fi บ้าน (LAN)** | อยู่บ้าน วงเดียวกับอุปกรณ์ | เรียก REST API ของ ESP32 ตรง ๆ (`/api/status`, `/api/*`) |
| **ทดลอง (Demo)** | ไม่มีอุปกรณ์ | จำลองข้อมูลในเครื่อง (สไลเดอร์ + ปุ่มเล่น) |

โหมดที่เลือกและค่าที่กรอกถูกจำไว้ใน `localStorage` เปิดแอปครั้งต่อไปจะเชื่อมต่อให้อัตโนมัติ

---

## โครงสร้างไฟล์

```
app/                     ← เว็บแอป (source ของ Capacitor, webDir)
  index.html             ← ทั้งแอปในไฟล์เดียว (UI + 3 โหมดเชื่อมต่อ)
  vendor/mqtt.min.js     ← ไลบรารี MQTT (โหลดในเครื่อง ทำงานออฟไลน์ได้)
  manifest.webmanifest   ← ข้อมูล PWA
  sw.js                  ← service worker (แคช UI, ไม่แคช MQTT/API)
  icons/                 ← ไอคอนแอป
capacitor.config.json    ← ตั้งค่า Capacitor (appId, webDir=app)
android/                 ← โปรเจกต์ Android (สร้างด้วย npx cap add android)
package.json             ← dependencies (@capacitor/*, mqtt)
```

---

## ตั้งค่า Cloud broker (ใช้ได้ทุกที่)

ใช้ **HiveMQ Cloud** (มีแพ็กเกจฟรี) หรือ broker MQTT ตัวใดก็ได้ที่รองรับ TLS + WebSocket

1. สมัคร HiveMQ Cloud → สร้าง cluster → สร้าง Access Credential (username/password)
2. จดค่า **Host** (เช่น `abcd1234.s1.eu.hivemq.cloud`)
3. **ฝั่ง ESP32** — เปิด `WATER_ALARM.ino` แก้ค่าด้านบนไฟล์ (หรือส่งผ่าน `/api/mqtt` ตอนต่อ LAN):
   ```cpp
   bool   mqttEnabled = true;
   String mqttHost    = "abcd1234.s1.eu.hivemq.cloud";
   int    mqttPort    = 8883;            // MQTT over TLS
   String mqttUser    = "your-user";
   String mqttPass    = "your-pass";
   String deviceId    = "wateralarm-1"; // ตั้งชื่อไม่ซ้ำ
   ```
   ต้องติดตั้ง library: **PubSubClient** และ **ArduinoJson (v7+)** ผ่าน Arduino Library Manager
4. **ฝั่งแอป** — หน้า ตั้งค่า → อุปกรณ์ → แท็บ “คลาวด์” กรอก:
   - Broker URL: `wss://abcd1234.s1.eu.hivemq.cloud:8884/mqtt`  ← **WebSocket (wss) พอร์ต 8884**
   - Device ID: `wateralarm-1`  ← **ต้องตรงกับ ESP32**
   - Username / Password: เหมือนที่ตั้งใน HiveMQ
   - กด “เชื่อมต่อคลาวด์”

> พอร์ตต่างกันตามโปรโตคอล: ESP32 ใช้ MQTT/TLS `8883`, แอป (เบราว์เซอร์/WebView) ใช้ WebSocket/TLS `8884`
> Topic ที่ใช้: `water/<deviceId>/status` (อุปกรณ์→แอป, retained) และ `water/<deviceId>/cmd` (แอป→อุปกรณ์)

---

## รันเว็บแอปเพื่อทดสอบ (ไม่ต้อง build เป็นแอป)

Service worker และ `fetch`/MQTT ต้องเสิร์ฟผ่าน HTTP ไม่ใช่ `file://`

```bash
python -m http.server 8777 --directory app
# เปิด http://localhost:8777/
```

(มี `.claude/launch.json` config ชื่อ `water-app` ให้รันผ่านเครื่องมือ preview ได้)

---

## Build เป็นแอปมือถือ (Capacitor)

ติดตั้ง dependencies ครั้งแรก:
```bash
npm install
npm run vendor      # คัดลอก mqtt.min.js เข้ามาที่ app/vendor (ทำให้แล้ว)
```

ทุกครั้งที่แก้ไฟล์ใน `app/` ให้ sync เข้าโปรเจกต์ native:
```bash
npx cap sync
```

### Android (build ได้บน Windows/Mac/Linux)
ต้องมี **Android Studio** (+ Android SDK)
```bash
npx cap open android      # เปิดใน Android Studio แล้วกด Run/Build APK
# หรือ command line:
cd android && ./gradlew assembleDebug
# ได้ไฟล์ที่ android/app/build/outputs/apk/debug/app-debug.apk
```

### iOS (ต้องใช้ macOS + Xcode)
```bash
npx cap add ios           # รันบน Mac เท่านั้น (ต้องมี CocoaPods)
npx cap open ios          # เปิด Xcode แล้ว Run ลงเครื่อง/ซิมูเลเตอร์
```

> โปรเจกต์ Android ถูกสร้างไว้แล้ว (`android/`) ส่วน iOS ให้รัน `npx cap add ios` บนเครื่อง Mac

---

## หมายเหตุ

- **Demo/Cloud ใช้ได้ทุกที่** ส่วน **LAN** ต้องอยู่ Wi-Fi วงเดียวกับ ESP32
- ESP32 ฝั่ง LAN เป็น HTTP ธรรมดา (มี CORS แล้ว) — ถ้าเปิดเว็บแอปผ่าน HTTPS จะโดน mixed-content
  block; ในแอป Capacitor ตั้ง `cleartext:true` ไว้ให้แล้ว จึงต่อ LAN ได้
- อย่าใช้ broker สาธารณะแบบไม่มีรหัสผ่านกับข้อมูลจริง (สถานะมี bot token/chat id) — ใช้ broker
  ที่มี auth เช่น HiveMQ Cloud
- ESP32 ยังเสิร์ฟหน้าเว็บในตัวที่ `http://<ip>/` ได้เหมือนเดิม

## แผนที่คำสั่ง (แอป → อุปกรณ์)

| การทำงาน | LAN (HTTP POST) | Cloud (MQTT publish → `water/<id>/cmd`) |
|----------|-----------------|------------------------------------------|
| เกณฑ์ | `/api/thresholds` | `{"action":"thresholds","high","med","low","hyst"}` |
| ข้อความ | `/api/messages` | `{"action":"messages","high","med","low"}` |
| Telegram | `/api/telegram` | `{"action":"telegram","token","chatId"}` |
| ทดสอบส่ง | `/api/test` | `{"action":"test"}` |
| รายงานเช้า | `/api/morning` | `{"action":"morning","enabled","hour","msg"}` |
| คาลิเบรต ADC | `/api/calib` | `{"action":"calib","adcMin","adcMax"}` |
| รีเซ็ต Wi-Fi | `/api/forget` | `{"action":"forget"}` |
| ตั้งค่า broker | `/api/mqtt` | — (ตั้งในแอป/ในสเก็ตช์) |

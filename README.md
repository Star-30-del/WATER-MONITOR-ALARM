# ระบบตรวจสอบน้ำ — Water Monitor

ระบบตรวจสอบระดับน้ำด้วย **ESP32** (เซนเซอร์ 4–20mA) + **เว็บแอป/PWA** ที่ใช้ได้ทั้ง iOS และ Android
จากทุกที่ผ่านคลาวด์ (MQTT) พร้อมแจ้งเตือน Telegram

## โครงสร้าง

| ส่วน | ที่อยู่ | รายละเอียด |
|------|--------|-----------|
| เว็บแอป (PWA) | [`app/`](app/) | UI ไฟล์เดียว 3 โหมด: คลาวด์ / Wi-Fi บ้าน / ทดลอง — ดู [app/README.md](app/README.md) |
| เฟิร์มแวร์ | [`WATER_ALARM.ino`](WATER_ALARM.ino) | ESP32: อ่านเซนเซอร์, HTTP API + CORS, MQTT cloud, Telegram |
| แอป native (ทางเลือก) | [`android/`](android/) | Capacitor wrapper — ใช้ต่อเมื่ออยากลงสโตร์ |

## ใช้งานเป็นเว็บแอป (แนะนำ — ง่ายสุด)

ไม่ต้อง build อะไร แค่เอา `app/` ขึ้นโฮสต์ HTTPS แล้วเปิดบนมือถือ → "เพิ่มลงหน้าจอโฮม"

### ดีพลอยขึ้น GitHub Pages (อัตโนมัติ)

Repo นี้มี workflow [`.github/workflows/deploy-pages.yml`](.github/workflows/deploy-pages.yml) ที่ดีพลอยโฟลเดอร์ `app/`
ให้เองทุกครั้งที่ push ขึ้น `main`

**ครั้งแรก** — สร้าง repo บน GitHub แล้ว push (แทน `<user>/<repo>` ด้วยของคุณ):

```bash
git remote add origin https://github.com/<user>/<repo>.git
git push -u origin main
```

จากนั้นบน GitHub: **Settings → Pages → Build and deployment → Source: GitHub Actions**
รอ workflow รันเสร็จ จะได้ลิงก์ `https://<user>.github.io/<repo>/`

> เปิดลิงก์บนมือถือ → เมนูแชร์ → "เพิ่มลงหน้าจอโฮม" ก็ได้แอปทั้ง iOS/Android

## เชื่อมต่ออุปกรณ์จากทุกที่ (คลาวด์)

ตั้ง cloud broker (เช่น HiveMQ Cloud ฟรี) ให้ทั้ง ESP32 และแอปใช้ **Device ID เดียวกัน**
ขั้นตอนละเอียดอยู่ใน [app/README.md](app/README.md)

## ทดสอบในเครื่อง

```bash
python -m http.server 8777 --directory app
# เปิด http://localhost:8777/
```

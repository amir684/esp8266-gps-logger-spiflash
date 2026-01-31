# ESP8266 GPS Logger with SPI Flash

Portable and robust GPS logger based on **ESP8266**, featuring an **external SPI Flash memory**, **OLED display**, and a **WiFi web interface** for downloading logs.

Designed for battery-powered operation, with power-loss-safe logging and full compatibility with **GPS Visualizer**.

---

## ✨ Features

- 📍 Logs GPS position **once per second**
- 💾 External SPI Flash (W25Q32 / W25Q64 / W25Q128)
- 🛡️ **Power-loss safe** logging (META START / FINAL mechanism)
- 🖥️ 0.96" OLED live display:
  - GPS Fix status
  - Speed (km/h)
  - Latitude / Longitude
  - Altitude
  - Satellites count
  - Free memory percentage
- 📶 WiFi **Access Point mode** with built-in web interface
- 🌐 Download logs directly from browser
- 🗂️ Multiple log files, timestamped by GPS time
- 🧹 Delete individual files or reset index
- 📄 TXT log format compatible with **gpsvisualizer.com**
- 🔋 Optimized for low power usage

---

## 🧰 Hardware

- ESP8266 module (ESP-12F recommended)
- GPS module (NEO-6M or compatible, UART)
- 0.96" OLED SSD1306 (I2C)
- External SPI Flash (Winbond W25Qxx series)
- 2 push buttons:
  - LOG (start / stop logging)
  - AP (WiFi access)
- 3.3V regulator (HT7333 recommended for battery use)
- Battery or external 3.7–5V supply

---

## 🔌 Wiring

### ESP8266 Connections

| Function        | ESP8266 Pin |
|----------------|------------|
| GPS TX         | RX (GPIO3) |
| OLED SDA       | GPIO4      |
| OLED SCL       | GPIO5      |
| Flash CS       | GPIO16     |
| Flash MOSI     | GPIO13     |
| Flash MISO     | GPIO12     |
| Flash SCK      | GPIO14     |
| LOG Button     | GPIO0 → GND |
| AP Button      | GPIO2 → GND |

> Buttons use internal pull-ups.

---

## ▶️ Usage

1. Power on the device
2. Wait for GPS fix
3. Press **LOG**:
   - If GPS time is not ready → `WAIT`
   - When ready → logging starts automatically
4. Press **LOG** again to stop and save the file
5. Press **AP** to enable WiFi access point
6. Connect to:

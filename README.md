# ESP8266 GPS Logger with SPI Flash

Portable and robust GPS logger based on **ESP8266**, featuring an **external SPI Flash memory**, **OLED display**, and a **WiFi web interface** for downloading logs.

Designed for battery-powered operation, with power-loss-safe logging and full compatibility with **GPS Visualizer**.

---


![image alt](https://github.com/amir684/esp8266-gps-logger-spiflash/blob/main/docs/ON.jpg)


---

## ✨ Features

- 📍 Logs GPS position **once per second**
- 💾 External SPI Flash (W25Q32 / W25Q64 / W25Q128)
- 🛡️ Power-loss safe logging (metadata journal + append-only data)
- 🖥️ 0.96" OLED live display:
  - Fix status (OK/NO)
  - Speed (km/h)
  - Latitude / Longitude
  - Altitude
  - Satellites count
  - Free memory percentage
  - Mode indicators (LOG/IDLE/WAIT + AP ON/OFF)
- 📶 WiFi **Access Point mode** with built-in web interface
- 🌐 Download logs directly from browser
- 🗂️ Multiple log files, timestamped by GPS time
- 🧹 Delete individual files (tombstone delete)
- ⚡ Fast "Erase ALL" (resets META index quickly)
- 📄 TXT format compatible with **gpsvisualizer.com**
- 🔋 Optimized for low power usage

---

## 🧰 Hardware

- ESP8266 module (ESP-12F recommended)
- GPS module (NEO-6M or compatible, UART)
- 0.96" OLED SSD1306 (I2C)
- External SPI Flash (Winbond W25Qxx series)
- 2 push buttons:
  - LOG (start / stop logging)
  - AP (enable/disable WiFi AP + Web UI)
- 3.3V regulator (HT7333 recommended for battery use)
- Battery or external 3.7–5V supply

> Tip: ESP8266 can have current peaks during WiFi activity. A good 3.3V regulator + decoupling capacitors improves stability.

---

![image alt](https://github.com/amir684/esp8266-gps-logger-spiflash/blob/main/docs/Back.jpg)

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

> Buttons use internal pull-ups (INPUT_PULLUP), so each button connects to GND when pressed.

### SPI Flash pins reminder (W25Qxx)
- CS  → GPIO16  
- CLK → GPIO14  
- DI  → GPIO13 (MOSI)  
- DO  → GPIO12 (MISO)  
- VCC → 3.3V  
- GND → GND  
- /HOLD and /WP → pull-up to 3.3V (or keep high) for standard SPI use.

---

## ▶️ Usage

1. Power on the device
2. Wait for GPS reception
3. Press **LOG**
   - If GPS time is not ready → `WAIT`
   - When GPS time becomes valid → logging starts automatically
4. Press **LOG** again to stop and finalize the file
5. Press **AP** to toggle WiFi AP + Web UI
6. Connect to AP:
   - SSID: `GPS-LOGGER`
   - Password: `12345678`
7. Open in browser:
   - `http://192.168.4.1`
8. Download files / delete single file / erase META index / exit AP mode

![image alt](https://github.com/amir684/esp8266-gps-logger-spiflash/blob/main/docs/web.png)

---

## ⚙️ Configuration (User Settings in Code)

These are the main settings at the top of the sketch:

### GPS
- `GPS_BAUD`
  - Defines the UART speed between GPS → ESP8266.
  - Many NEO-6 modules default to **9600**.
  - If your module is set to **115200**, change this accordingly.

### SPI Flash
- `SPI_HZ`
  - SPI clock for flash access.
  - Typical stable values: **1–4 MHz** (depends on wiring quality).

- `FLASH_SIZE_BYTES`
  - Must match your external flash capacity:
    - W25Q32  = 4MB  → `0x400000`
    - W25Q64  = 8MB  → `0x800000`
    - W25Q128 = 16MB → `0x1000000`
    - W25Q256 = 32MB → `0x2000000`

> Wiring stays identical. Only `FLASH_SIZE_BYTES` changes.

### WiFi AP
- `AP_SSID`, `AP_PASS`
  - Access point credentials for downloading logs.

### Pins
- OLED: SDA=GPIO4, SCL=GPIO5  
- Buttons: LOG=GPIO0, AP=GPIO2  
- Flash CS: GPIO16  
(Other SPI pins use ESP8266 hardware SPI pins)

---

## 🧠 How It Works (Deep Dive)

### 1) Memory Layout

External flash is split into two logical areas:

- **META (index / journal)**  
  - `META_START = 0x00000`  
  - `META_END   = 0x10000` (64KB)

- **DATA (actual log data)**  
  - `DATA_START = 0x10000`  
  - `DATA_END   = FLASH_SIZE_BYTES`

This design keeps file indexing robust and makes erase operations fast.

---

### 2) The META Journal (Why it is safe)

META uses fixed-size records (256 bytes each).  
Each record includes:
- Magic + version (validation)
- Record type (file record or delete record)
- Unique session ID (seq)
- Start address + size
- GPS timestamp
- File name
- CRC checksum (detects partial writes)

#### Record types
- `REC_FILE`  
  A completed file entry: startAddr + size + name + timestamp.

- `REC_DEL`  
  Tombstone delete: marks a specific seq as deleted without rewriting old META.

**Important:** META is append-only.  
That means:
- even if power drops mid-write, the next boot can scan until the last valid CRC record and ignore the broken tail.

---

### 3) DATA writing strategy (No page-cross bugs)

Flash writes use **Page Program** (256 bytes page).  
The code ensures it never crosses a page boundary:

- `dataWriteStream()` splits data into chunks that fit inside the current page
- `ensureErasedForAddr()` erases 4KB sectors *only when needed* (erase-on-demand)

Result:
- stable writes
- no corrupt page-boundary writes
- fast enough for 1Hz logging

---

### 4) What happens when power is lost mid-log?

Because DATA is written continuously, losing power can stop in the middle of a write.

What you get:
- The partially-written DATA remains in flash (that's normal)
- The file may NOT have a final META file record yet (because stopLogNow() didn't run)

On next boot:
- `loadMetaIndex()` rebuilds the file list **only from valid META records**
- The incomplete data region is effectively "unindexed"
- New logs continue from the next safe write pointer

✅ No full-memory corruption  
✅ No “everything is lost” scenario  
✅ Only the current active log might be incomplete

---

### 5) What happens when DATA reaches the end?

When `dataWriteAddr >= DATA_END`:
- logging is stopped safely (the code prevents writing past end)
- freePercent() becomes 0
- next logs will not start until you erase/reset META (and restart from beginning)

---

## 📄 Log Format (GPS Visualizer compatible)

Each log file begins with a required header row:


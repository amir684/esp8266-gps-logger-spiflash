/*
  ESP8266 GPS Logger (SPI Flash) — TXT + KML Export
  -------------------------------------------------
  Portable GPS logger based on ESP8266 with:
  - External SPI Flash (W25Qxx) for robust, power-loss-safe logging
  - 0.96" SSD1306 OLED status display
  - WiFi AP web interface for downloading logs

  Log formats:
  - TXT/CSV stored in flash (GPS Visualizer compatible)
  - KML generated on-demand (streamed) with speed-colored track segments
    + Start/End markers (Google Earth ready)

  Pins (ESP8266 / ESP-12F):
  - GPS TX  -> RX (GPIO3)
  - OLED SDA -> GPIO4
  - OLED SCL -> GPIO5
  - FLASH CS -> GPIO16
  - FLASH SCK -> GPIO14
  - FLASH MOSI -> GPIO13
  - FLASH MISO -> GPIO12
  - LOG Button -> GPIO0 to GND (INPUT_PULLUP)
  - AP  Button -> GPIO2 to GND (INPUT_PULLUP)

  Notes:
  - Buttons use internal pull-ups: connect button to GND when pressed.
  - SPI Flash /HOLD and /WP should be pulled up to 3.3V for normal SPI use.
  - KML export does not consume additional flash storage (generated from TXT on download).

  Copyright (c) 2026 AmirY
  License: Apache License 2.0 (see LICENSE file)
*/


struct RgbColor { unsigned char r, g, b; };

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#include <TinyGPSPlus.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ---------------- USER SETTINGS ----------------
static const uint32_t GPS_BAUD = 9600;
static const uint32_t SPI_HZ   = 2000000;

const char* AP_SSID = "GPS-LOGGER";
const char* AP_PASS = "12345678";

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static const uint8_t PIN_OLED_SDA = 4; // GPIO4
static const uint8_t PIN_OLED_SCL = 5; // GPIO5

// ---------------- Buttons ----------------
static const uint8_t PIN_BTN_LOG = 0; // GPIO0 -> GND
static const uint8_t PIN_BTN_AP  = 2; // GPIO2 -> GND

// ---------------- GPS ----------------
TinyGPSPlus gps;

// ---------------- External SPI Flash ----------------
static const uint8_t  PIN_FLASH_CS     = 16; // GPIO16
static const uint32_t EXT_SECTOR_SIZE  = 4096;
static const uint32_t EXT_PAGE_SIZE    = 256;
static const uint32_t FLASH_SIZE_BYTES = 0x400000; // 4MB (W25Q32)

// Layout:
// META: 0x00000..0x0FFFF (64KB) 256B records
// DATA: 0x10000..end
static const uint32_t META_START = 0x00000;
static const uint32_t META_END   = 0x10000;
static const uint32_t DATA_START = 0x10000;
static const uint32_t DATA_END   = FLASH_SIZE_BYTES;

// Record types
static const uint32_t META_MAGIC = 0x4D455441; // 'META'
static const uint16_t META_VER   = 1;

static const uint16_t REC_FINAL  = 1; // completed file record
static const uint16_t REC_DEL    = 2; // delete tombstone
static const uint16_t REC_START  = 3; // log started (in-progress)

struct __attribute__((packed)) MetaRecordBase {
  uint32_t magic;
  uint16_t ver;
  uint16_t type;       // REC_START / REC_FINAL / REC_DEL
  uint32_t seq;        // unique session id (monotonic)
  uint32_t startAddr;  // for START/FINAL: data start; for DEL: target seq
  uint32_t size;       // for FINAL: bytes; for START: 0; for DEL: 0
  uint16_t year;
  uint8_t  month, day;
  uint8_t  hour, minute, second;
  char     name[32];   // filename
  uint32_t crc;
};

struct __attribute__((packed)) MetaRecord256 {
  MetaRecordBase b;
  uint8_t pad[256 - sizeof(MetaRecordBase)];
};

static_assert(sizeof(MetaRecordBase) <= 256, "MetaRecordBase too big");
static_assert(sizeof(MetaRecord256) == 256, "MetaRecord256 must be 256 bytes");

// In-RAM file list
static const uint16_t MAX_FILES = 240;

struct FileEntry {
  uint32_t seq;
  uint32_t startAddr;
  uint32_t size;
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  char     name[32];
  bool     incomplete; // NEW
};

FileEntry files[MAX_FILES];
uint16_t fileCount = 0;

// Pointers
uint32_t metaWriteAddr = META_START;
uint32_t dataWriteAddr = DATA_START;
uint32_t nextEraseSectorBase = DATA_START;

// Logging state
bool logging = false;
bool pendingStart = false;

uint32_t curStartAddr = 0;
uint32_t curSize = 0;
uint32_t curSeq = 0;

uint16_t curYear = 0;
uint8_t  curMo=0, curDa=0, curHh=0, curMm=0, curSs=0;
char     curName[32] = {0};

uint8_t  buf[512];
uint16_t bufLen = 0;

int lastLoggedSecond = -1;

// Full/Finalize handling
bool finalizeNeeded = false;
bool fullStop = false;

// Web/AP
ESP8266WebServer server(80);
bool apMode = false;

// ---------------- SPI Flash helpers ----------------
static inline void flashSelect()   { digitalWrite(PIN_FLASH_CS, LOW); }
static inline void flashDeselect() { digitalWrite(PIN_FLASH_CS, HIGH); }

uint8_t flashReadStatus() {
  flashSelect();
  SPI.transfer(0x05); // RDSR
  uint8_t s = SPI.transfer(0x00);
  flashDeselect();
  return s;
}

void flashWaitBusy() {
  while (flashReadStatus() & 0x01) { delay(1); yield(); }
}

void flashWriteEnable() {
  flashSelect();
  SPI.transfer(0x06); // WREN
  flashDeselect();
}

void flashReadBytes(uint32_t addr, uint8_t* out, size_t len) {
  flashSelect();
  SPI.transfer(0x03); // READ
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) out[i] = SPI.transfer(0x00);
  flashDeselect();
}
// ---------------- KML export helpers ----------------
//struct RgbColor { uint8_t r, g, b; };

static RgbColor hsvToRgb(float h, float s, float v) {
  // h: 0..360, s/v: 0..1
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;

  float rp = 0, gp = 0, bp = 0;
  if      (h < 60)  { rp = c; gp = x; bp = 0; }
  else if (h < 120) { rp = x; gp = c; bp = 0; }
  else if (h < 180) { rp = 0; gp = c; bp = x; }
  else if (h < 240) { rp = 0; gp = x; bp = c; }
  else if (h < 300) { rp = x; gp = 0; bp = c; }
  else              { rp = c; gp = 0; bp = x; }

  RgbColor out;
  out.r = (uint8_t)((rp + m) * 255.0f + 0.5f);
  out.g = (uint8_t)((gp + m) * 255.0f + 0.5f);
  out.b = (uint8_t)((bp + m) * 255.0f + 0.5f);
  return out;
}

// KML uses AABBGGRR hex. We'll map 0 km/h -> red (hue 0°) and vmax -> magenta (hue 300°).
static void speedToKmlColor(char out[9], float spd, float vmax) {
  if (vmax < 1.0f) vmax = 1.0f;
  float t = spd / vmax;
  if (t < 0) t = 0;
  if (t > 1) t = 1;

  float hue = 0.0f + 300.0f * t;   // 0..300
  RgbColor c = hsvToRgb(hue, 1.0f, 0.90f);

  // out = "ffBBGGRR"
  snprintf(out, 9, "ff%02x%02x%02x", c.b, c.g, c.r);
}

// Read one CSV line from external flash (handles CRLF). Returns false if nothing left.
static bool readLineFromFlash(uint32_t &addr, uint32_t endAddr, char *line, size_t maxLen) {
  size_t i = 0;
  while (addr < endAddr && i < maxLen - 1) {
    uint8_t ch;
    flashReadBytes(addr, &ch, 1);
    addr++;

    if (ch == '\n') break;
    if (ch == '\r') continue;
    line[i++] = (char)ch;
  }
  line[i] = 0;
  return (i > 0) || (addr < endAddr);
}

static bool parseCsvLine(const char *line, float &lat, float &lon, int &alt, int &spd) {
  // expected: lat,lon,elevation,time,speed_kmh
  // time contains spaces, so parse the 4th field as "everything until next comma"
  char timebuf[40];
  int n = sscanf(line, "%f,%f,%d,%39[^,],%d", &lat, &lon, &alt, timebuf, &spd);
  return (n >= 5);
}

void flashSectorErase4K(uint32_t sectorBase) {
  flashWriteEnable();
  flashSelect();
  SPI.transfer(0x20); // 4KB erase
  SPI.transfer((sectorBase >> 16) & 0xFF);
  SPI.transfer((sectorBase >> 8) & 0xFF);
  SPI.transfer(sectorBase & 0xFF);
  flashDeselect();
  flashWaitBusy();
}

// NOTE: Page Program must NOT cross page boundary!
void flashPageProgram(uint32_t addr, const uint8_t* data, size_t len) {
  flashWriteEnable();
  flashSelect();
  SPI.transfer(0x02); // PP
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) SPI.transfer(data[i]);
  flashDeselect();
  flashWaitBusy();
}

bool isAllFF(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] != 0xFF) return false;
  return true;
}

uint32_t crcSimple(const uint8_t* p, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s = (s * 131) + p[i];
  return s;
}

// ---------------- Erase-on-demand for DATA ----------------
void ensureErasedForAddr(uint32_t addr) {
  uint32_t sectorBase = (addr / EXT_SECTOR_SIZE) * EXT_SECTOR_SIZE;
  while (nextEraseSectorBase <= sectorBase && nextEraseSectorBase < DATA_END) {
    flashSectorErase4K(nextEraseSectorBase);
    nextEraseSectorBase += EXT_SECTOR_SIZE;
    yield();
  }
}

// Robust stream writer: never crosses page boundary.
void dataWriteStream(uint32_t &addr, const uint8_t* data, size_t len) {
  while (len > 0) {
    if (addr >= DATA_END) {
      // reached end: request finalize
      logging = false;
      finalizeNeeded = true;
      fullStop = true;
      return;
    }

    ensureErasedForAddr(addr);

    uint32_t pageOff = addr & (EXT_PAGE_SIZE - 1);
    uint16_t room    = (uint16_t)(EXT_PAGE_SIZE - pageOff);
    uint16_t chunk   = (len < room) ? (uint16_t)len : room;

    // safety: do not exceed DATA_END
    if (addr + chunk > DATA_END) chunk = (uint16_t)(DATA_END - addr);

    if (chunk == 0) {
      logging = false;
      finalizeNeeded = true;
      fullStop = true;
      return;
    }

    flashPageProgram(addr, data, chunk);

    addr += chunk;
    data += chunk;
    len  -= chunk;

    curSize += chunk;

    // if we exactly hit end, stop & finalize
    if (addr >= DATA_END) {
      logging = false;
      finalizeNeeded = true;
      fullStop = true;
      return;
    }
    yield();
  }
}

// Buffering on top of stream writer
void logAppend(const uint8_t* data, size_t len) {
  if (!logging || len == 0) return;

  while (len > 0 && logging) {
    uint16_t space = (uint16_t)(sizeof(buf) - bufLen);
    uint16_t n = (len < space) ? (uint16_t)len : space;
    memcpy(buf + bufLen, data, n);
    bufLen += n;
    data += n;
    len  -= n;

    if (bufLen >= 256) {
      dataWriteStream(dataWriteAddr, buf, bufLen);
      bufLen = 0;
    }
  }
}

void flushLog() {
  if (bufLen) {
    dataWriteStream(dataWriteAddr, buf, bufLen);
    bufLen = 0;
  }
}

// ---------------- META helpers ----------------
void makeName(char out[32], uint16_t year, uint8_t mo, uint8_t da, uint8_t hh, uint8_t mm, uint8_t ss) {
  snprintf(out, 32, "GPS_%02u%02u%02u_%02u%02u%02u.TXT",
           (unsigned)((year >= 2000) ? (year - 2000) : year),
           (unsigned)mo, (unsigned)da,
           (unsigned)hh, (unsigned)mm, (unsigned)ss);
}

bool gpsTimeValid() { return gps.date.isValid() && gps.time.isValid(); }

// Write any META record (256B slot)
bool writeMetaRecord(uint16_t type, uint32_t seq, uint32_t startAddr, uint32_t size,
                     uint16_t year, uint8_t mo, uint8_t da, uint8_t hh, uint8_t mm, uint8_t ss,
                     const char* nameOrNull) {
  if (metaWriteAddr + EXT_PAGE_SIZE > META_END) return false;

  MetaRecord256 rec;
  memset(&rec, 0xFF, sizeof(rec));

  rec.b.magic = META_MAGIC;
  rec.b.ver   = META_VER;
  rec.b.type  = type;
  rec.b.seq   = seq;
  rec.b.startAddr = startAddr;
  rec.b.size      = size;

  rec.b.year = year; rec.b.month = mo; rec.b.day = da;
  rec.b.hour = hh;   rec.b.minute = mm; rec.b.second = ss;

  if (nameOrNull) {
    strncpy(rec.b.name, nameOrNull, sizeof(rec.b.name));
    rec.b.name[sizeof(rec.b.name)-1] = 0;
  } else {
    rec.b.name[0] = 0;
  }

  rec.b.crc = crcSimple((const uint8_t*)&rec.b, offsetof(MetaRecordBase, crc));

  flashPageProgram(metaWriteAddr, (const uint8_t*)&rec, sizeof(rec));
  metaWriteAddr += EXT_PAGE_SIZE;
  return true;
}

bool writeMetaRecord_DEL(uint32_t targetSeq) {
  // DEL uses its own seq (monotonic)
  uint32_t delSeq = ++curSeq;

  uint16_t y = 0; uint8_t mo=0, da=0, hh=0, mm=0, ss=0;
  if (gpsTimeValid()) {
    y = gps.date.year(); mo = gps.date.month(); da = gps.date.day();
    hh = gps.time.hour(); mm = gps.time.minute(); ss = gps.time.second();
  }

  // store target seq in startAddr field
  return writeMetaRecord(REC_DEL, delSeq, targetSeq, 0, y, mo, da, hh, mm, ss, nullptr);
}

// ---------------- Free space % ----------------
uint8_t freePercent() {
  uint32_t total = (DATA_END - DATA_START);
  uint32_t freeB = (DATA_END > dataWriteAddr) ? (DATA_END - dataWriteAddr) : 0;
  if (!total) return 0;
  uint32_t pct = (freeB * 100UL) / total;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

// ---------------- File list / META scanning ----------------
void removeBySeq(uint32_t seq) {
  for (uint16_t i = 0; i < fileCount; i++) {
    if (files[i].seq == seq) {
      for (uint16_t j = i + 1; j < fileCount; j++) files[j - 1] = files[j];
      fileCount--;
      return;
    }
  }
}

int findIndexBySeq(uint32_t seq) {
  for (uint16_t i = 0; i < fileCount; i++) if (files[i].seq == seq) return (int)i;
  return -1;
}

// For ASCII logs: find first erased region (0xFF) after start, estimate size
uint32_t estimateSizeFromData(uint32_t startAddr) {
  const uint32_t CH = 256;
  uint8_t tmp[CH];

  uint32_t addr = startAddr;
  while (addr < DATA_END) {
    flashReadBytes(addr, tmp, CH);

    if (isAllFF(tmp, CH)) {
      // first empty block; size ends at first FF inside this block (usually at block start)
      return (addr - startAddr);
    }

    // If partially empty: find first 0xFF byte (should not appear in text)
    for (uint32_t i = 0; i < CH; i++) {
      if (tmp[i] == 0xFF) {
        return (addr - startAddr) + i;
      }
    }

    addr += CH;
    yield();
  }
  return (DATA_END - startAddr);
}

void loadMetaIndex() {
  fileCount = 0;
  metaWriteAddr = META_START;
  dataWriteAddr = DATA_START;

  // curSeq = max seq seen in META (for monotonic numbering)
  curSeq = 0;

  MetaRecord256 rec;
  for (uint32_t addr = META_START; addr < META_END; addr += EXT_PAGE_SIZE) {
    flashReadBytes(addr, (uint8_t*)&rec, sizeof(rec));

    if (isAllFF((const uint8_t*)&rec, sizeof(rec))) {
      metaWriteAddr = addr;
      break;
    }

    if (rec.b.magic != META_MAGIC || rec.b.ver != META_VER) {
      metaWriteAddr = addr;
      break;
    }

    uint32_t expect = crcSimple((const uint8_t*)&rec.b, offsetof(MetaRecordBase, crc));
    if (rec.b.crc != expect) {
      metaWriteAddr = addr;
      break;
    }

    if (rec.b.seq > curSeq) curSeq = rec.b.seq;

    if (rec.b.type == REC_START) {
      // add as incomplete (size will be estimated on boot)
      int idx = findIndexBySeq(rec.b.seq);
      if (idx < 0 && fileCount < MAX_FILES) {
        FileEntry &e = files[fileCount++];
        e.seq = rec.b.seq;
        e.startAddr = rec.b.startAddr;
        e.size      = 0;
        e.year      = rec.b.year;
        e.month     = rec.b.month;
        e.day       = rec.b.day;
        e.hour      = rec.b.hour;
        e.minute    = rec.b.minute;
        e.second    = rec.b.second;
        strncpy(e.name, rec.b.name, sizeof(e.name));
        e.name[sizeof(e.name)-1] = 0;
        e.incomplete = true;
      }
    }
    else if (rec.b.type == REC_FINAL) {
      int idx = findIndexBySeq(rec.b.seq);
      if (idx >= 0) {
        // finalize existing (was START)
        FileEntry &e = files[idx];
        e.startAddr = rec.b.startAddr;
        e.size      = rec.b.size;
        e.year      = rec.b.year;
        e.month     = rec.b.month;
        e.day       = rec.b.day;
        e.hour      = rec.b.hour;
        e.minute    = rec.b.minute;
        e.second    = rec.b.second;
        strncpy(e.name, rec.b.name, sizeof(e.name));
        e.name[sizeof(e.name)-1] = 0;
        e.incomplete = false;
      } else if (fileCount < MAX_FILES) {
        // FINAL without START (still accept)
        FileEntry &e = files[fileCount++];
        e.seq = rec.b.seq;
        e.startAddr = rec.b.startAddr;
        e.size      = rec.b.size;
        e.year      = rec.b.year;
        e.month     = rec.b.month;
        e.day       = rec.b.day;
        e.hour      = rec.b.hour;
        e.minute    = rec.b.minute;
        e.second    = rec.b.second;
        strncpy(e.name, rec.b.name, sizeof(e.name));
        e.name[sizeof(e.name)-1] = 0;
        e.incomplete = false;
      }
    }
    else if (rec.b.type == REC_DEL) {
      // DEL: startAddr holds target seq
      removeBySeq(rec.b.startAddr);
    }
    else {
      metaWriteAddr = addr;
      break;
    }
  }

  // Fixup sizes for incomplete & compute dataWriteAddr = max endAddr
  dataWriteAddr = DATA_START;
  for (uint16_t i = 0; i < fileCount; i++) {
    FileEntry &e = files[i];
    if (e.incomplete) {
      e.size = estimateSizeFromData(e.startAddr);
      // mark name clearly (doesn't change stored name, only RAM display)
      // (We'll show it in HTML/UI by flag)
    }

    uint32_t endAddr = e.startAddr + e.size;
    if (endAddr > dataWriteAddr) dataWriteAddr = endAddr;
  }

  if (dataWriteAddr < DATA_START) dataWriteAddr = DATA_START;

  // next erase pointer follows current write pointer
  nextEraseSectorBase = (dataWriteAddr / EXT_SECTOR_SIZE) * EXT_SECTOR_SIZE;
  if (nextEraseSectorBase < DATA_START) nextEraseSectorBase = DATA_START;
}

// ---------------- Start/Stop logging ----------------
void startLogNow() {
  if (!gpsTimeValid()) { pendingStart = true; return; }
  if (metaWriteAddr + EXT_PAGE_SIZE > META_END) return;
  if (dataWriteAddr + 64 > DATA_END) return;

  logging = true;
  pendingStart = false;
  finalizeNeeded = false;
  fullStop = false;

  bufLen = 0;
  lastLoggedSecond = -1;

  curStartAddr = dataWriteAddr;
  curSize = 0;

  // freeze start time & filename from GPS
  curYear = gps.date.year();
  curMo   = gps.date.month();
  curDa   = gps.date.day();
  curHh   = gps.time.hour();
  curMm   = gps.time.minute();
  curSs   = gps.time.second();
  makeName(curName, curYear, curMo, curDa, curHh, curMm, curSs);

  // new session id
  curSeq = curSeq + 1;
  uint32_t sessionSeq = curSeq;

  // write META START immediately (power-loss safe)
  bool ok = writeMetaRecord(REC_START, sessionSeq, curStartAddr, 0,
                            curYear, curMo, curDa, curHh, curMm, curSs,
                            curName);
  if (!ok) {
    logging = false;
    return;
  }

  // set current seq
  curSeq = sessionSeq;

  nextEraseSectorBase = (dataWriteAddr / EXT_SECTOR_SIZE) * EXT_SECTOR_SIZE;
  if (nextEraseSectorBase < DATA_START) nextEraseSectorBase = DATA_START;

  // Header for GPS Visualizer
  const char* header = "latitude,longitude,elevation,time,speed_kmh\r\n";
  logAppend((const uint8_t*)header, strlen(header));
  flushLog(); // safe
}

void finalizeCurrentLog() {
  // finalize even if already stopped due to FULL
  flushLog();
  logging = false;
  pendingStart = false;

  // write META FINAL (same seq, same name/time as start)
  // curSeq is the session seq
  writeMetaRecord(REC_FINAL, curSeq, curStartAddr, curSize,
                  curYear, curMo, curDa, curHh, curMm, curSs,
                  curName);

  loadMetaIndex();
}

void stopLogNow() {
  finalizeCurrentLog();
}

// ---------------- Quick Erase All (META only) ----------------
void eraseAllQuick() {
  if (logging) { flushLog(); logging = false; }
  pendingStart = false;
  finalizeNeeded = false;
  fullStop = false;

  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.println("Erasing META...");
  display.display();

  for (uint32_t a = META_START; a < META_END; a += EXT_SECTOR_SIZE) {
    flashSectorErase4K(a);
    yield();
  }

  metaWriteAddr = META_START;
  dataWriteAddr = DATA_START;
  nextEraseSectorBase = DATA_START;
  curSeq = 0;
  fileCount = 0;

  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Erase META OK");
  display.display();
}

// ---------------- Buttons ----------------
bool pressed(uint8_t pin) {
  static uint32_t lastMs0 = 0, lastMs2 = 0;
  static bool last0 = true, last2 = true;

  bool cur = digitalRead(pin);
  uint32_t &lm = (pin == PIN_BTN_LOG) ? lastMs0 : lastMs2;
  bool &lp     = (pin == PIN_BTN_LOG) ? last0  : last2;

  if (cur != lp) { lm = millis(); lp = cur; }
  if (!cur && (millis() - lm) > 40) {
    while (!digitalRead(pin)) { delay(5); yield(); }
    return true;
  }
  return false;
}

// ---------------- Log one line per second ----------------
void tryLogOneLinePerSecond() {
  if (!logging) return;
  if (!gps.date.isValid() || !gps.time.isValid()) return;
  if (!gps.location.isValid()) return;

  int sec = (int)gps.time.second();
  if (sec == lastLoggedSecond) return;
  lastLoggedSecond = sec;

  // Most compatible for GPSVisualizer:
  char t[24];
  snprintf(t, sizeof(t), "%04u-%02u-%02u %02u:%02u:%02u",
           gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second());

  // Save altitude and speed as INT (rounded) to save space
  int spd = gps.speed.isValid() ? (int)(gps.speed.kmph() + 0.5f) : 0;
  int alt = gps.altitude.isValid() ? (int)(gps.altitude.meters() + (gps.altitude.meters() >= 0 ? 0.5f : -0.5f)) : 0;

  char line[128];
  snprintf(line, sizeof(line),
           "%.6f,%.6f,%d,%s,%d\r\n",
           gps.location.lat(), gps.location.lng(),
           alt, t, spd);

  logAppend((const uint8_t*)line, strlen(line));

  static uint32_t lastFlushMs = 0;
  if (millis() - lastFlushMs > 1500) {
    lastFlushMs = millis();
    flushLog();
  }
}

// ---------------- Web UI ----------------
String htmlIndex() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>GPS Logger</title><style>";
  s += "body{font-family:system-ui,Arial;margin:16px;background:#0b0f14;color:#e8eef6}";
  s += ".card{max-width:900px;margin:auto;background:#121a23;border:1px solid #243245;border-radius:16px;padding:16px}";
  s += "h2{margin:0 0 10px 0}";
  s += ".muted{color:#9fb0c3}";
  s += "table{width:100%;border-collapse:collapse;margin-top:12px}";
  s += "th,td{padding:10px;border-bottom:1px solid #243245;text-align:left}";
  s += "a.btn{display:inline-block;padding:10px 12px;border-radius:12px;text-decoration:none;color:#0b0f14;background:#79d2ff;font-weight:800}";
  s += "a.danger{background:#ff7d7d}";
  s += ".row{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px}";
  s += ".tag{display:inline-block;padding:2px 8px;border-radius:999px;background:#223247;color:#cfe2ff;font-weight:700;font-size:12px;margin-left:8px}";
  s += "</style></head><body><div class='card'>";
  s += "<h2>GPS Logger</h2>";
  s += "<div class='muted'>Files: " + String(fileCount) +
       " | Free: " + String(freePercent()) + "% (" + String((DATA_END - dataWriteAddr) / 1024) + " KB)</div>";

  s += "<div class='row'>";
  s += "<a class='btn' href='/status'>Status</a>";
  s += "<a class='btn danger' href='/erase_all' onclick=\"return confirm('Erase ALL (META reset)?')\">Erase ALL</a>";
  s += "<a class='btn' href='/exit'>Exit AP</a>";
  s += "</div>";

  s += "<table><tr><th>#</th><th>Name</th><th>Size</th><th>Download</th><th>Delete</th></tr>";
  for (int i = (int)fileCount - 1, shown = 0; i >= 0 && shown < 90; --i, ++shown) {
    s += "<tr><td>" + String(i) + "</td>";
    s += "<td>" + String(files[i].name);
    if (files[i].incomplete) s += "<span class='tag'>INCOMPLETE</span>";
    s += "</td>";
    s += "<td>" + String(files[i].size) + " B</td>";
    s += "<td><a class='btn' href='/download?i=" + String(i) + "'>TXT</a> <a class='btn' href='/download_kml?i=" + String(i) + "'>KML</a></td>";
    s += "<td><a class='btn danger' href='/delete?i=" + String(i) + "' onclick=\"return confirm('Delete this file?')\">Delete</a></td></tr>";
  }
  s += "</table></div></body></html>";
  return s;
}

void stopAPServer() {
  server.stop();
  WiFi.softAPdisconnect(true);
  delay(50);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
  apMode = false;
}

void startAPServer() {
  // keep your stability choice: stop logging when entering AP
  //if (logging) stopLogNow();

  apMode = true;

  WiFi.forceSleepWake();
  delay(1);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  delay(1);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlIndex()); });

  server.on("/status", HTTP_GET, []() {
    String s;
    s += "Files: " + String(fileCount) + "\n";
    s += "Meta write: 0x" + String(metaWriteAddr, HEX) + "\n";
    s += "Data write: 0x" + String(dataWriteAddr, HEX) + "\n";
    s += "Free: " + String(freePercent()) + "%\n";
    server.send(200, "text/plain", s);
  });

  server.on("/erase_all", HTTP_GET, []() {
    server.send(200, "text/plain", "Erasing META (fast). Please wait...");
    delay(10);
    eraseAllQuick();
    loadMetaIndex();
  });

  server.on("/delete", HTTP_GET, []() {
    if (!server.hasArg("i")) { server.send(400, "text/plain", "Missing i"); return; }
    int idx = server.arg("i").toInt();
    if (idx < 0 || idx >= (int)fileCount) { server.send(404, "text/plain", "Bad index"); return; }

    uint32_t targetSeq = files[idx].seq;

    bool ok = writeMetaRecord_DEL(targetSeq);
    if (!ok) { server.send(500, "text/plain", "META full; cannot delete"); return; }

    loadMetaIndex();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/exit", HTTP_GET, []() {
    server.send(200, "text/plain", "Exiting AP... close this tab.");
    delay(50);
    stopAPServer();
  });

  server.on("/download", HTTP_GET, []() {
    if (!server.hasArg("i")) { server.send(400, "text/plain", "Missing i"); return; }
    int idx = server.arg("i").toInt();
    if (idx < 0 || idx >= (int)fileCount) { server.send(404, "text/plain", "Bad index"); return; }

    const FileEntry &e = files[idx];
    if (e.size == 0) { server.send(200, "text/plain", "Empty file"); return; }

    WiFiClient client = server.client();
    client.setNoDelay(true);

    client.printf(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Disposition: attachment; filename=%s\r\n"
      "Content-Length: %lu\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n\r\n",
      e.name,
      (unsigned long)e.size
    );

    const size_t CHUNK = 256;
    uint8_t tmp[CHUNK];

    uint32_t remaining = e.size;
    uint32_t addr = e.startAddr;

    while (remaining > 0) {
      size_t n = (remaining > CHUNK) ? CHUNK : remaining;
      flashReadBytes(addr, tmp, n);

      size_t w = client.write(tmp, n);
      if (w != n) break;

      addr += n;
      remaining -= n;
      delay(0);
    }
    client.stop();
  });
  server.on("/download_kml", HTTP_GET, []() {
    if (!server.hasArg("i")) { server.send(400, "text/plain", "Missing i"); return; }
    int idx = server.arg("i").toInt();
    if (idx < 0 || idx >= (int)fileCount) { server.send(404, "text/plain", "Bad index"); return; }

    const FileEntry &e = files[idx];
    if (e.size == 0) { server.send(200, "text/plain", "Empty file"); return; }

    WiFiClient client = server.client();
    client.setNoDelay(true);

    // Stream response (no Content-Length) so we don't buffer a whole KML in RAM.
    client.printf(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/vnd.google-earth.kml+xml\r\n"
      "Content-Disposition: attachment; filename=%s.kml\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n\r\n",
      e.name
    );

    uint32_t addr = e.startAddr;
    uint32_t endAddr = e.startAddr + e.size;
    char line[180];

    // PASS 1: find vmax
    // Skip header
    readLineFromFlash(addr, endAddr, line, sizeof(line));

    int vmax = 0;
    unsigned long lastYield = millis();
    int pointCount = 0;
    while (addr < endAddr) {
      if (!readLineFromFlash(addr, endAddr, line, sizeof(line))) break;
      float lat, lon; int alt, spd;
      if (!parseCsvLine(line, lat, lon, alt, spd)) continue;
      if (spd > vmax) vmax = spd;

      // Aggressive watchdog feeding: every 50 points OR every 1 second
      pointCount++;
      if (pointCount >= 50 || (millis() - lastYield) >= 1000) {
        ESP.wdtFeed();
        yield();
        lastYield = millis();
        pointCount = 0;
      }
    }
    if (vmax < 1) vmax = 1;

    // KML header
    client.print("<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n");
    client.print("<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n");
    client.print("  <Document>\n");
    client.printf("    <name><![CDATA[%s]]></name>\n", e.name);
    client.print("    <open>1</open>\n");

    // Legend (5 points: 0, vmax/4, vmax/2, 3vmax/4, vmax)
    client.print("    <Folder>\n");
    client.print("      <name>Legend: Speed (km/h)</name>\n");
    for (int k = 4; k >= 0; k--) {
      float sVal = (vmax * (float)k) / 4.0f;
      char kmlColor[9];
      speedToKmlColor(kmlColor, sVal, (float)vmax);

      // Build HTML color "#RRGGBB" from "ffBBGGRR"
      uint8_t bb = (uint8_t)strtoul(kmlColor + 2, nullptr, 16);
      uint8_t gg = (uint8_t)strtoul(kmlColor + 4, nullptr, 16);
      uint8_t rr = (uint8_t)strtoul(kmlColor + 6, nullptr, 16);

      char htmlHex[8];
      snprintf(htmlHex, sizeof(htmlHex), "%02x%02x%02x", rr, gg, bb);

      client.print("      <Placemark>\n");
      client.printf("        <name><![CDATA[<span style=\"color:#%s;\"><b>%.1f</b></span>]]></name>\n", htmlHex, sVal);
      client.print("      </Placemark>\n");
      delay(0);
    }
    client.print("    </Folder>\n");

    // Tracks
    client.print("    <Folder>\n");
    client.print("      <name>Tracks</name>\n");
    client.print("      <Folder>\n");
    client.printf("        <name>%s</name>\n", e.name);

    // PASS 2: segments + Start/End
    addr = e.startAddr;
    readLineFromFlash(addr, endAddr, line, sizeof(line)); // header again

    float prevLat = 0, prevLon = 0; int prevAlt = 0, prevSpd = 0;
    bool havePrev = false;

    bool haveStart = false;
    float startLat = 0, startLon = 0; int startAlt = 0;
    float endLat = 0, endLon = 0; int endAlt = 0;

    lastYield = millis();
    pointCount = 0;

    while (addr < endAddr) {
      if (!readLineFromFlash(addr, endAddr, line, sizeof(line))) break;

      float lat, lon; int alt, spd;
      if (!parseCsvLine(line, lat, lon, alt, spd)) continue;

      if (!haveStart) {
        startLat = lat; startLon = lon; startAlt = alt;
        haveStart = true;
      }
      endLat = lat; endLon = lon; endAlt = alt;

      if (!havePrev) {
        prevLat = lat; prevLon = lon; prevAlt = alt; prevSpd = spd;
        havePrev = true;
        continue;
      }

      float sUse = 0.5f * (prevSpd + spd); // smoother colors
      char kmlColor[9];
      speedToKmlColor(kmlColor, sUse, (float)vmax);

      client.print("        <Placemark>\n");
      client.print("          <Style><LineStyle>\n");
      client.printf("            <color>%s</color>\n", kmlColor);
      client.print("            <width>4</width>\n");
      client.print("          </LineStyle></Style>\n");
      client.print("          <LineString>\n");
      client.print("            <tessellate>1</tessellate>\n");
      client.print("            <altitudeMode>clampToGround</altitudeMode>\n");
      client.print("            <coordinates>");
      // KML order: lon,lat,alt
      client.printf("%.6f,%.6f,%d %.6f,%.6f,%d", prevLon, prevLat, prevAlt, lon, lat, alt);
      client.print("</coordinates>\n");
      client.print("          </LineString>\n");
      client.print("        </Placemark>\n");

      prevLat = lat; prevLon = lon; prevAlt = alt; prevSpd = spd;

      // Aggressive watchdog feeding: every 50 points OR every 1 second
      pointCount++;
      if (pointCount >= 50 || (millis() - lastYield) >= 1000) {
        ESP.wdtFeed();
        yield();
        lastYield = millis();
        pointCount = 0;
      }
    }

    // Start/End markers
    if (haveStart) {
      client.print("        <Placemark><name>Start</name><Point><coordinates>");
      client.printf("%.6f,%.6f,%d", startLon, startLat, startAlt);
      client.print("</coordinates></Point></Placemark>\n");

      client.print("        <Placemark><name>End</name><Point><coordinates>");
      client.printf("%.6f,%.6f,%d", endLon, endLat, endAlt);
      client.print("</coordinates></Point></Placemark>\n");
    }

    client.print("      </Folder>\n");
    client.print("    </Folder>\n");
    client.print("  </Document>\n");
    client.print("</kml>\n");

    client.stop();
  });

  server.begin();
}

// ---------------- UI ----------------
void drawUI() {
  bool fix = gps.location.isValid() && gps.location.age() < 2000;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Top-left: LOG/IDLE + AP:ON/OFF
  display.setCursor(0, 0);

  bool blink = ((millis() / 1000) % 2) == 0;  // ON/OFF כל שנייה
  if (logging) {
    if (blink) display.print("LOG ");
    else       display.print("    ");         // 4 רווחים במקום LOG
  } else if (pendingStart) {
  display.print("WAIT");
  } else {
    display.print("IDLE");
  }


  display.print(" ");
  display.print("AP:");
  display.print(apMode ? "ON" : "OFF");

  // Top-right MEM
  display.setCursor(75, 0);
  display.print("MEM:");
  display.print(freePercent());
  display.print("%");

  int y = 12;

  display.setCursor(0, y);
  display.print("Fix:");
  display.print(fix ? "OK" : "NO");
  display.print("   Spd:");
  display.setTextSize(2);
  display.print(gps.speed.isValid() ? (int)(gps.speed.kmph() + 0.5) : 0);

  int16_t x = display.getCursorX();
  display.setTextSize(1);
  display.setCursor(x, y + 8);
  display.print("kmh");

  y += 12;

  display.setTextSize(1);
  display.setCursor(0, y);
  display.print("Lat:");
  if (gps.location.isValid()) display.print(gps.location.lat(), 6);
  else display.print("---");

  y += 12;

  display.setCursor(0, y);
  display.print("Lon:");
  if (gps.location.isValid()) display.print(gps.location.lng(), 6);
  else display.print("---");

  y += 12;

  display.setCursor(0, y);
  display.print("SAT:");
  display.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
  display.print("   Alt:");
  display.print(gps.altitude.isValid() ? (int)(gps.altitude.meters() + (gps.altitude.meters() >= 0 ? 0.5f : -0.5f)) : 0);

  // Small FULL indicator if we stopped due to full
  if (fullStop) {
    display.setCursor(90, 54);
    display.print("FULL");
  }

  display.display();
}

// ---------------- Setup/Loop ----------------
void setup() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);

  pinMode(PIN_FLASH_CS, OUTPUT);
  digitalWrite(PIN_FLASH_CS, HIGH);

  pinMode(PIN_BTN_LOG, INPUT_PULLUP);
  pinMode(PIN_BTN_AP,  INPUT_PULLUP);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("GPS Logger");
  display.println("Init...");
  display.display();

  SPI.begin();
  SPI.setFrequency(SPI_HZ);
  SPI.setDataMode(SPI_MODE0);
  flashWaitBusy();

  loadMetaIndex();

  Serial.begin(GPS_BAUD);

  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Ready");
  display.println("BTN0: LOG");
  display.println("BTN2: AP");
  display.display();
}

void loop() {
  while (Serial.available()) {
    gps.encode((char)Serial.read());
  }

  if (pressed(PIN_BTN_LOG)) {
    if (!logging && !pendingStart) {
      if (gpsTimeValid()) startLogNow();
      else pendingStart = true;
    } else {
      if (logging) stopLogNow();
      else pendingStart = false;
    }
  }

  if (pendingStart && gpsTimeValid()) startLogNow();

  if (pressed(PIN_BTN_AP)) {
    if (!apMode) startAPServer();
    else stopAPServer();
  }

  if (apMode) server.handleClient();

  tryLogOneLinePerSecond();

  // finalize if we were forced to stop (full end)
  if (finalizeNeeded) {
    finalizeNeeded = false;
    finalizeCurrentLog();
  }

  static uint32_t lastUI = 0;
  if (millis() - lastUI > 250) {
    lastUI = millis();
    drawUI();
  }
}
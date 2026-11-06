// ESP32-S3 = AP + web server + log SD + Modbus RTU ke Omron E5CC (RS-485).
// Hold dijalankan oleh Omron (Soak Time + Program Pattern STOP).
// Logic Operation CX-Thermo (PRG.E OR LBA) dibaca dari SUB1 (0x2002).
// Status LBA murni dibaca dari 0x2001 bit 13.
// Jam: RTC DS3231 = sumber utama, jam HP (lewat web) = cadangan.
//
// v4:
//   - Pematik manual = picu register Burner ON (Program Start) selama 0,3 detik, lalu otomatis
//     Burner OFF (Program Reset). Sisanya (valve menutup, pematik nyala ~20 dtk) dikerjakan program Omron.
//   - Output serial USB 1 Hz berformat  $LOG,<datetime>,<temp>,<setpoint>,<ON|OFF>  sebagai backup logging
//     (dibaca script serial_logger.py di laptop). Baris lain di serial (debug) diabaikan script itu.
//
// LOGGING PER PERCOBAAN:
//   - Tiap percobaan = 1 file CSV di SD: /logs/<nama>.csv
//   - Sebelum Start, operator wajib membuat/memilih file lewat halaman Log (/api/log/init)
//   - Sesi log "dipakai habis" begitu logging selesai -> percobaan berikutnya harus init lagi
//   - Jika SD tidak terbaca, Start ditolak kecuali operator konfirmasi (nosd=1) -> jalan tanpa logging
#include <WiFi.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <RTClib.h>              // Library Manager: "RTClib" by Adafruit
#include <ESPAsyncWebServer.h>
#include <ModbusMaster.h>
#include <sys/time.h>
#include <time.h>
#include <esp_partition.h>

const char* AP_SSID = "Burner-ESP";
const char* AP_PASS = "burner1234";

// ---- RTC (I2C) ----
const int RTC_SDA = 8, RTC_SCL = 9;      // sesuaikan dengan wiring (8/9 = default I2C ESP32-S3)
RTC_DS3231 rtc;
bool    rtcPresent = false;
uint8_t rtcState = 0;                    // 0=tidak terdeteksi, 1=OK, 2=jam hilang (baterai habis)
uint8_t timeSrc  = 0;                    // 0=belum, 1=RTC, 2=HP
const char* TSTR[] = {"none", "RTC", "HP"};
const char* RSTR[] = {"none", "ok", "lost"};

// ---- Modbus / Omron ----
const int      RS485_RX   = 18;
const int      RS485_TX   = 17;
const int      RS485_CTRL = 4;

const uint8_t  OMRON_ID = 1;
const uint16_t REG_STATUS_UPPER = 0x2001; // Status alarm internal (Bit 13 = status murni LBA)
const uint16_t REG_STATUS_SUB1  = 0x2002; // Bit 8 = SUB1 (hasil Logic Operation: PRG.E OR LBA)
const uint16_t REG_PV        = 0x2000;
const uint16_t REG_SP        = 0x2103;
const uint16_t REG_MV        = 0x2004;
const uint16_t REG_SOAK_REM  = 0x2728;
const uint16_t REG_SOAK_SET  = 0x2729;
const uint16_t REG_SOAK_UNIT = 0x3327;
const uint16_t REG_PTRN      = 0x2D16;

const uint16_t REG_CMD       = 0x0000;
const uint16_t CMD_PR_RESET  = 0x1100;   // Program Reset (berfungsi sebagai STOP)
const uint16_t CMD_PRST      = 0x1101;   // Program Start (berfungsi sebagai RUN)

const bool     STOP_ON_BOOT  = true;

// ---- Pematik manual ----
const uint16_t IGN_PULSE_MS = 300;       // lama register Burner ON sebelum dimatikan lagi
const uint8_t  IGN_OMRON_S  = 20;        // lama pematik nyala menurut program Omron (hanya untuk tampilan + kunci tombol)

// Omron tampil tanpa desimal (48 = 48 C) -> SCALE 1.
// Jika di Omron diatur 1 desimal (48.0), kembalikan ke 10.0f.
const float    SCALE    = 1.0f;
const float    MV_SCALE = 10.0f;
const uint8_t  MAX_FAIL = 5;
const uint32_t MB_PERIOD_MS = 100;

const float  MAX_SP   = 1200;
const long   MAX_HOLD = 5999;
const float  TOL      = 2.0f;
const float  ON_T = 50.0f, OFF_T = 48.0f;
const char*  LEGACY_LOG = "/log.csv";    // sisa versi lama di LittleFS (dihapus saat boot)
const char*  LOGS_DIR   = "/logs";       // folder log di SD
const int    MAX_LOG_FILES = 60;
const int    SD_SCK = 12, SD_MISO = 13, SD_MOSI = 11, SD_CS = 10;
const bool   REQUIRE_SD_TO_START = true; // true = tanpa SD harus konfirmasi operator (nosd=1)
const char*  LOG_HDR  = "datetime,temp_c,burner_on_s,burner_status\n";
const char*  PHASE[]  = {"IDLE", "HEATING", "HOLD"};

struct State {
  float pv = 0.0f, sp = 0;
  bool run = false, valve = false, burner = false, logging = false;
  uint8_t phase = 0;
  uint8_t ign = 0;
  uint32_t onS = 0, holdTotal = 0, holdLeft = 0;
  char event[48] = "Boot";
} S;

// (Variabel ini dipertahankan agar respon JSON API web tidak berubah)
uint16_t lbaTimeS   = 120;
float    lbaBand    = 2.0f;
float    lbaMvLevel = 90.0f;
float    holdBand   = 5.0f;
uint16_t holdOutS   = 60;
bool     lbaAlarm   = false;             // SUB1 (0x2002 bit 8): dipakai untuk auto-stop
bool     lbaOmron   = false;             // LBA murni (0x2001 bit 13): ditampilkan di web
bool     sub1Alarm  = false;             // status SUB1 untuk web
uint16_t lbaTmr = 0, outTmr = 0;
float    lbaPvRef = 0;
float    mvPct = -1;

AsyncWebServer server(80);
SemaphoreHandle_t mtx;
volatile bool dlBusy = false;
bool timeSet = false;
SPIClass sdSPI(FSPI);
uint8_t sdSt = 1;
const char* SDSTR[] = {"ok", "missing", "error"};
uint32_t lastSdTry = 0;

// ---- Sesi log aktif (diisi lewat /api/log/init) ----
char logName[33]     = "";               // kosong = belum ada sesi log
char logPathBuf[64]  = "";
bool resumeSep       = false;            // tulis baris "# RESUME <waktu>" sebelum baris data pertama

ModbusMaster node;
bool    commOk = false;
bool    sensorOk = true;
uint8_t commFail = 0;
uint8_t lastErr = 0xFF;
uint8_t lastWErr = 0xFF;
int16_t lastRaw = 0;
float   spOmron = -1;

bool     startReq = false;
float    reqSp = 0;
uint16_t reqHold = 0;
bool     stopPending = false;
volatile bool ignReq  = false;           // permintaan pematik manual dari web
volatile bool ignBusy = false;           // pulsa pematik sedang dikirim
bool     bootStopDone = false;
uint16_t soakRem = 0xFFFF;
bool     soakArmed = false;
bool     seenFull = false;
uint32_t p2Sec = 0;

void preTransmission()  { digitalWrite(RS485_CTRL, HIGH); }
void postTransmission() { digitalWrite(RS485_CTRL, LOW);  }

void setEvent(const char* e) { strlcpy(S.event, e, sizeof(S.event)); }

void nowStr(char* out, size_t n) {
  if (!timeSet) { snprintf(out, n, "NOTIME"); return; }
  time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

// Set jam sistem dari epoch (UTC). src: 1=RTC, 2=HP. Tulis balik ke RTC bila dari HP.
void applyTime(uint32_t e, uint8_t src) {
  struct timeval tv = { (time_t)e, 0 };
  settimeofday(&tv, nullptr);
  timeSet = true;
  timeSrc = src;
  if (src == 2 && rtcPresent) {
    rtc.adjust(DateTime(e));
    rtcState = 1;                        // flag lostPower dibersihkan oleh adjust()
  }
}

// ---- Helper nama file log ----
// Nama hanya boleh huruf, angka, underscore (1-32 karakter). Ini juga mencegah path traversal.
bool validName(const String& n) {
  if (n.length() < 1 || n.length() > 32) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

String pathFor(const String& name) { return String(LOGS_DIR) + "/" + name + ".csv"; }

void setActiveLog(const char* n) {
  strlcpy(logName, n, sizeof(logName));
  snprintf(logPathBuf, sizeof(logPathBuf), "%s/%s.csv", LOGS_DIR, n);
}

void clearActiveLog() { logName[0] = 0; logPathBuf[0] = 0; resumeSep = false; }

bool sdMount() {
  SD.end();
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 4000000) && SD.cardType() != CARD_NONE) {
    if (!SD.exists(LOGS_DIR)) SD.mkdir(LOGS_DIR);
    sdSt = 0; return true;
  }
  sdSt = 1; return false;
}

// Ambil ukuran file (false jika SD bermasalah / file tidak ada)
bool fileInfo(const char* path, size_t& size) {
  if (sdSt != 0 || !SD.exists(path)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size = f.size(); f.close();
  return true;
}

// Ambil timestamp (19 karakter) dari baris terakhir file yang sudah terbuka
void lastStamp(File& f, char* out, size_t n) {
  out[0] = 0;
  size_t sz = f.size();
  if (sz == 0 || n < 20) return;
  size_t pos = sz > 200 ? sz - 200 : 0;
  char b[201];
  if (!f.seek(pos)) return;
  size_t got = f.read((uint8_t*)b, sz - pos);
  b[got] = 0;
  while (got > 0 && (b[got - 1] == '\n' || b[got - 1] == '\r')) b[--got] = 0;
  char* ls = strrchr(b, '\n');
  ls = ls ? ls + 1 : b;
  while (*ls && !(*ls >= '0' && *ls <= '9')) ls++;     // lewati "# RESUME " dst.
  if (strlen(ls) >= 19) { memcpy(out, ls, 19); out[19] = 0; }
}

void writeRow() {
  if (sdSt != 0 || !logPathBuf[0]) return;
  bool fresh = !SD.exists(logPathBuf);
  File f = SD.open(logPathBuf, FILE_APPEND);
  if (!f) { sdSt = 2; setEvent("SD error: gagal buka log"); return; }
  if (fresh) f.print(LOG_HDR);
  char ts[24], line[80];
  nowStr(ts, sizeof(ts));
  if (resumeSep) { f.printf("# RESUME %s\n", ts); resumeSep = false; }
  snprintf(line, sizeof(line), "%s,%.1f,%u,%s\n", ts, S.pv, (unsigned)S.onS, S.burner ? "ON" : "OFF");
  size_t w = f.print(line);
  f.close();
  if (w == 0) { sdSt = 2; setEvent("SD error: gagal tulis"); }
}

void doStop(const char* reason) {
  S.run = false; S.phase = 0; S.valve = false; S.ign = 0;
  startReq = false; soakArmed = false;
  stopPending = true;
  setEvent(reason);
}

void gap() { vTaskDelay(pdMS_TO_TICKS(30)); }

bool startSequence(float sp, uint16_t holdMin, char* why, size_t n) {
  uint8_t e = node.readHoldingRegisters(REG_SOAK_UNIT, 1);
  if (e != node.ku8MBSuccess) { snprintf(why, n, "Baca soak unit gagal 0x%02X", e); return false; }
  if (node.getResponseBuffer(0) != 0) { snprintf(why, n, "Soak unit bukan menit (0x3327)"); return false; }
  gap();
  e = node.readHoldingRegisters(REG_PTRN, 1);
  if (e != node.ku8MBSuccess) { snprintf(why, n, "Baca PtRn gagal 0x%02X", e); return false; }
  if (node.getResponseBuffer(0) == 0) { snprintf(why, n, "PtRn OFF: set STOP di Omron"); return false; }
  gap();
  e = node.writeSingleRegister(REG_SP, (uint16_t)(int16_t)lroundf(sp * SCALE));
  lastWErr = e;
  if (e != node.ku8MBSuccess) { snprintf(why, n, "Gagal tulis SP 0x%02X", e); return false; }
  gap();
  e = node.writeSingleRegister(REG_SOAK_SET, holdMin);
  lastWErr = e;
  if (e != node.ku8MBSuccess) { snprintf(why, n, "Gagal tulis soak 0x%02X", e); return false; }
  gap();

  // Langsung kirim Program Start
  e = node.writeSingleRegister(REG_CMD, CMD_PRST);
  lastWErr = e;
  if (e != node.ku8MBSuccess) { snprintf(why, n, "Gagal perintah PRST 0x%02X", e); return false; }

  return true;
}

// Pematik manual: Burner ON (PRST) -> tunggu 0,3 dtk -> Burner OFF (PR_RESET). Dipanggil dari modbusTask.
// Catatan: bila Omron telat membalas, timeout Modbus (default 2 dtk) bisa memperpanjang jeda ON->OFF.
// OFF SELALU dikirim (3x coba); bila tetap gagal, stopPending dinyalakan agar STOP diulang sampai berhasil.
void igniterPulse() {
  uint8_t eOn = node.writeSingleRegister(REG_CMD, CMD_PRST);
  uint32_t tOn = millis();
  uint32_t el = millis() - tOn;
  while (el < IGN_PULSE_MS) {
    vTaskDelay(pdMS_TO_TICKS(IGN_PULSE_MS - el));
    el = millis() - tOn;
  }
  uint8_t eOff = 0xFF;
  for (uint8_t i = 0; i < 3; i++) {
    eOff = node.writeSingleRegister(REG_CMD, CMD_PR_RESET);
    if (eOff == node.ku8MBSuccess) break;
    gap();
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  lastWErr = (eOff != node.ku8MBSuccess) ? eOff : eOn;
  if (eOn == node.ku8MBSuccess) S.ign = IGN_OMRON_S;
  if (eOff != node.ku8MBSuccess) {
    stopPending = true;
    setEvent("Pematik: OFF gagal, STOP diulang");
  } else if (eOn != node.ku8MBSuccess) {
    snprintf(S.event, sizeof(S.event), "Pematik gagal: ON error 0x%02X", eOn);
  } else {
    setEvent("Pematik manual dipicu (0,3 dtk)");
  }
  ignBusy = false;
  xSemaphoreGive(mtx);
}

void modbusTask(void*) {
  uint8_t cyc = 0;
  for (;;) {
    uint32_t t0 = millis();

    bool doStart = false, doStopCmd = false, doIgn = false; float sp = 0; uint16_t hm = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (stopPending) doStopCmd = true;
    else if (startReq) { doStart = true; sp = reqSp; hm = reqHold; startReq = false; }
    else if (ignReq) { ignReq = false; if (!S.run) { doIgn = true; ignBusy = true; } }
    xSemaphoreGive(mtx);

    if (doStopCmd) {
      // Program Reset sebagai perintah Stop
      uint8_t e = node.writeSingleRegister(REG_CMD, CMD_PR_RESET);
      xSemaphoreTake(mtx, portMAX_DELAY);
      lastWErr = e;
      if (e == node.ku8MBSuccess) stopPending = false;
      xSemaphoreGive(mtx);
      vTaskDelay(pdMS_TO_TICKS(30));
    } else if (doIgn) {
      igniterPulse();
    } else if (doStart) {
      char why[48];
      bool ok = startSequence(sp, hm, why, sizeof(why));
      xSemaphoreTake(mtx, portMAX_DELAY);
      if (ok) { if (S.run) soakArmed = true; }
      else if (S.run) doStop(why);
      xSemaphoreGive(mtx);
    }

    // 2) baca PV (tiap siklus)
    uint8_t r = node.readHoldingRegisters(REG_PV, 1);
    xSemaphoreTake(mtx, portMAX_DELAY);
    lastErr = r;
    if (r == node.ku8MBSuccess) {
      lastRaw = (int16_t)node.getResponseBuffer(0);
      float pv = lastRaw / SCALE;
      commOk = true; commFail = 0;
      sensorOk = (pv > -250 && pv < 1500);
      if (sensorOk) S.pv = pv;
      if (STOP_ON_BOOT && !bootStopDone && !S.run) { stopPending = true; bootStopDone = true; }
    } else {
      if (commFail < 250) commFail++;
      if (commFail >= MAX_FAIL) commOk = false;
    }
    xSemaphoreGive(mtx);

    // 3) baca register lain bergantian
    cyc = (cyc + 1) % 8;
    if (cyc == 1) {
      if (node.readHoldingRegisters(REG_SP, 1) == node.ku8MBSuccess) {
        float v = (int16_t)node.getResponseBuffer(0) / SCALE;
        xSemaphoreTake(mtx, portMAX_DELAY); spOmron = v; xSemaphoreGive(mtx);
      }
    } else if (cyc == 3) {
      if (node.readHoldingRegisters(REG_MV, 1) == node.ku8MBSuccess) {
        float m = (int16_t)node.getResponseBuffer(0) / MV_SCALE;
        xSemaphoreTake(mtx, portMAX_DELAY); mvPct = m; xSemaphoreGive(mtx);
      }
    } else if (cyc == 5) {
      if (node.readHoldingRegisters(REG_SOAK_REM, 1) == node.ku8MBSuccess) {
        uint16_t v = node.getResponseBuffer(0);
        xSemaphoreTake(mtx, portMAX_DELAY);
        soakRem = v;
        if (S.run && soakArmed && v > 0) seenFull = true;
        xSemaphoreGive(mtx);
      }
    } else if (cyc == 7) {
      // ALARM OMRON: baca 2 register berurutan sekaligus
      //   [0] = 0x2001 -> bit 13 = LBA murni
      //   [1] = 0x2002 -> bit 8  = SUB1 (Logic Operation: PRG.E OR LBA)
      if (node.readHoldingRegisters(REG_STATUS_UPPER, 2) == node.ku8MBSuccess) {
        uint16_t up  = node.getResponseBuffer(0);
        uint16_t sub = node.getResponseBuffer(1);
        bool lbaNow = bitRead(up, 13);
        bool subNow = bitRead(sub, 8);

        xSemaphoreTake(mtx, portMAX_DELAY);
        lbaOmron  = lbaNow;
        sub1Alarm = subNow;
        if (S.run && lbaNow) {
          lbaAlarm = true;
          doStop("ALARM LBA Omron (loop break)");
        } else if (S.run && subNow) {
          lbaAlarm = true;
          doStop("ALARM OMRON AKTIF (Hold Habis / LBA)");
        } else if (!subNow) {
          lbaAlarm = false;
        }
        xSemaphoreGive(mtx);
      }
    }

    uint32_t el = millis() - t0;
    vTaskDelay(pdMS_TO_TICKS(el < MB_PERIOD_MS ? MB_PERIOD_MS - el : 1));
  }
}

void tick() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (sdSt != 0 && millis() - lastSdTry > 5000) { lastSdTry = millis(); sdMount(); }
  if (S.ign > 0) S.ign--;

  if (S.run && !commOk)   doStop("Komunikasi Omron putus");
  if (S.run && !sensorOk) doStop("Sensor Omron error");

  bool prevValve = S.valve;
  if (S.run) {
    if (S.pv < S.sp - TOL) S.valve = true;
    else if (S.pv >= S.sp) S.valve = false;
  }
  if (S.valve && !prevValve) S.ign = 4;

  if (!S.burner && S.pv > ON_T) S.burner = true;
  else if (S.burner && S.pv <= OFF_T) S.burner = false;
  if (S.burner) S.onS++;

  // ---- FASE DAN HOLD ----
  if (S.run) {
    if (seenFull) {
      if (S.phase == 1) { S.phase = 2; p2Sec = 0; }
    }

    if (seenFull && soakRem == 0) {
      doStop("Tahan selesai (soak Omron)");
    } else if (S.phase == 1) {
      if (S.pv >= S.sp - TOL) { S.phase = 2; p2Sec = 0; setEvent("Menahan SP"); }
    } else if (S.phase == 2) {
      p2Sec++;

      // 1. Hitung mundur lokal per detik agar timer web berjalan mulus
      if (S.holdLeft > 0) S.holdLeft--;

      // 2. Sinkronisasi dengan Omron (resolusi menit vs detik)
      if (soakRem != 0xFFFF && soakRem > 0) {
        uint32_t omronCeil  = (uint32_t)soakRem * 60;
        uint32_t omronFloor = ((uint32_t)soakRem - 1) * 60 + 1;
        if (S.holdLeft < omronFloor) S.holdLeft = omronFloor;       // Omron pause -> tahan timer web
        else if (S.holdLeft > omronCeil) S.holdLeft = omronCeil;    // tertinggal -> paksa turun
      }

      if (fabsf(S.pv - S.sp) <= holdBand) { outTmr = 0; setEvent("Menahan SP (soak Omron)"); }
      else {
        setEvent("Hold: suhu di luar band");
        if (++outTmr >= holdOutS) doStop("Suhu keluar band hold");
      }
      if (p2Sec > S.holdTotal + 180) doStop("Soak Omron tidak selesai");
    }
  }
  if (S.logging) {
    writeRow();
    // Logging selesai -> sesi log "habis dipakai"; percobaan berikutnya harus init lagi
    if (!S.run && !S.burner) { S.logging = false; clearActiveLog(); }
  }
  xSemaphoreGive(mtx);
}

// Backup logging via USB serial (1 Hz): $LOG,<datetime>,<temp_c>,<setpoint_c>,<ON|OFF>
// datetime = "NOTIME" bila jam belum ter-set (script laptop menggantinya dengan jam laptop).
void emitSerialLog() {
  char ts[24];
  xSemaphoreTake(mtx, portMAX_DELAY);
  nowStr(ts, sizeof(ts));
  float pv = S.pv;
  float sp = (spOmron >= 0) ? spOmron : S.sp;
  bool bn = S.burner;
  xSemaphoreGive(mtx);
  Serial.printf("$LOG,%s,%.1f,%.1f,%s\n", ts, pv, sp, bn ? "ON" : "OFF");
}

void sendErr(AsyncWebServerRequest* r, int code, const char* msg) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
  r->send(code, "application/json", buf);
}

void setupRoutes() {
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest* r) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"heap\":%u}", (unsigned)ESP.getFreeHeap());
    r->send(200, "application/json", buf);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    char buf[1024], ts[24];
    xSemaphoreTake(mtx, portMAX_DELAY);
    nowStr(ts, sizeof(ts));
    const char* om = !commOk ? "error" : (!sensorOk ? "sensor" : "ok");
    snprintf(buf, sizeof(buf),
      "{\"pv\":%.1f,\"sp\":%.0f,\"run\":%s,\"phase\":\"%s\",\"valve\":\"%s\",\"igniter\":\"%s\",\"burner\":\"%s\","
      "\"on_s\":%u,\"hold_total_s\":%u,\"hold_left_s\":%u,\"sd\":\"%s\",\"omron\":\"%s\",\"mb_err\":\"0x%02X\","
      "\"sp_omron\":%.1f,\"werr\":\"0x%02X\",\"mv\":%.1f,\"lba\":\"%s\",\"lba_tmr\":%u,\"soak_rem\":%d,"
      "\"lba_omron\":\"%s\",\"sub1\":\"%s\",\"time_src\":\"%s\",\"rtc\":\"%s\","
      "\"log\":\"%s\",\"logging\":%s,"
      "\"event\":\"%s\",\"ts\":\"%s\"}",
      S.pv, S.sp, S.run ? "true" : "false", PHASE[S.phase], S.valve ? "ON" : "OFF",
      S.ign > 0 ? "ON" : "OFF", S.burner ? "ON" : "OFF", (unsigned)S.onS, (unsigned)S.holdTotal, (unsigned)S.holdLeft,
      SDSTR[sdSt], om, lastErr, spOmron, lastWErr, mvPct, lbaAlarm ? "alarm" : "ok", (unsigned)lbaTmr,
      soakRem == 0xFFFF ? -1 : (int)soakRem,
      lbaOmron ? "alarm" : "ok", sub1Alarm ? "alarm" : "ok", TSTR[timeSrc], RSTR[rtcState],
      logName, S.logging ? "true" : "false",
      S.event, ts);
    xSemaphoreGive(mtx);
    r->send(200, "application/json", buf);
  });

  server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("sp")) { sendErr(r, 400, "Input tidak valid"); return; }
    float sp = r->getParam("sp")->value().toFloat();
    long hold = r->hasParam("hold_min") ? r->getParam("hold_min")->value().toInt() : 0;
    bool noSd = r->hasParam("nosd");        // operator sudah konfirmasi: jalan tanpa logging
    if (!(sp > 0 && sp <= MAX_SP)) { sendErr(r, 400, "SP harus 1-1200"); return; }
    if (hold < 1 || hold > MAX_HOLD) { sendErr(r, 400, "Tahan minimal 1 menit, maks 99 jam 59 menit"); return; }
    const char* err = nullptr;
    bool needConfirm = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (S.run) err = "Burner sudah berjalan";
    else if (ignReq || ignBusy) err = "Pematik sedang dipicu, coba lagi sebentar";
    else if (!timeSet) err = "Jam belum ter-set, muat ulang halaman";
    else if (!commOk) err = "Omron tidak merespons";
    else if (!sensorOk) err = "Sensor/input Omron error";
    else if (REQUIRE_SD_TO_START && sdSt != 0 && !noSd) { err = "SD card tidak terdeteksi"; needConfirm = true; }
    else if (sdSt == 0 && !logName[0]) err = "Belum ada sesi log. Buat CSV dulu di halaman Log";
    else {
      bool withLog = (sdSt == 0);
      S.sp = sp; S.run = true; S.phase = 1; S.onS = 0; S.logging = withLog;
      S.holdTotal = hold * 60; S.holdLeft = hold * 60;
      lbaAlarm = false; lbaTmr = 0; outTmr = 0; p2Sec = 0;
      seenFull = false; soakArmed = false; soakRem = 0xFFFF;
      stopPending = false;
      reqSp = sp; reqHold = (uint16_t)hold; startReq = true;
      if (withLog) {
        snprintf(S.event, sizeof(S.event), "Start SP %.0f, tahan %02ld:%02ld", sp, hold / 60, hold % 60);
        writeRow();
      } else {
        clearActiveLog();
        snprintf(S.event, sizeof(S.event), "Start SP %.0f, %02ld:%02ld, tanpa log", sp, hold / 60, hold % 60);
      }
    }
    xSemaphoreGive(mtx);
    if (err) {
      char b[200];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\",\"need_sd_confirm\":%s}", err, needConfirm ? "true" : "false");
      r->send(409, "application/json", b);
    } else r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest* r) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (S.run) doStop("Manual stop");
    xSemaphoreGive(mtx);
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Pematik manual: picu Burner ON 0,3 dtk lalu OFF otomatis (dikerjakan modbusTask).
  // Durasi nyala pematik diatur program Omron; tombol dikunci selama IGN_OMRON_S detik.
  server.on("/api/igniter", HTTP_POST, [](AsyncWebServerRequest* r) {
    const char* err = nullptr;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (S.run) err = "Tidak bisa manual saat sesi berjalan";
    else if (ignReq || ignBusy) err = "Pematik sedang dipicu";
    else if (S.ign > 0) err = "Pematik masih menyala, tunggu selesai";
    else if (!commOk) err = "Omron tidak merespons";
    else if (stopPending) err = "Perintah stop ke Omron belum selesai";
    else ignReq = true;
    xSemaphoreGive(mtx);
    if (err) sendErr(r, 409, err);
    else r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/cfg", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest* r) {
    const char* err = nullptr;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (r->method() == HTTP_POST) {
      if (S.run) err = "Sesi sedang berjalan";
      else {
        if (r->hasParam("lba_s"))      { long v = r->getParam("lba_s")->value().toInt();        if (v >= 10 && v <= 3600) lbaTimeS = v; else err = "lba_s 10-3600"; }
        if (r->hasParam("lba_band"))   { float v = r->getParam("lba_band")->value().toFloat();  if (v >= 0.5f && v <= 50) lbaBand = v; else err = "lba_band 0.5-50"; }
        if (r->hasParam("lba_mv"))     { float v = r->getParam("lba_mv")->value().toFloat();    if (v >= 50 && v <= 100) lbaMvLevel = v; else err = "lba_mv 50-100"; }
        if (r->hasParam("hold_band"))  { float v = r->getParam("hold_band")->value().toFloat(); if (v >= 1 && v <= 50) holdBand = v; else err = "hold_band 1-50"; }
        if (r->hasParam("hold_out_s")) { long v = r->getParam("hold_out_s")->value().toInt();   if (v >= 10 && v <= 3600) holdOutS = v; else err = "hold_out_s 10-3600"; }
      }
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
      "{\"ok\":%s,\"lba_s\":%u,\"lba_band\":%.1f,\"lba_mv\":%.0f,\"hold_band\":%.1f,\"hold_out_s\":%u}",
      err ? "false" : "true", (unsigned)lbaTimeS, lbaBand, lbaMvLevel, holdBand, (unsigned)holdOutS);
    xSemaphoreGive(mtx);
    if (err) sendErr(r, 400, err);
    else r->send(200, "application/json", buf);
  });

  // Jam HP = cadangan: hanya dipakai jika RTC belum menyediakan waktu.
  server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest* r) {
    long e = r->hasParam("epoch") ? r->getParam("epoch")->value().toInt() : 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (e > 1700000000L && !timeSet) applyTime((uint32_t)e, 2);
    bool ok = timeSet;
    xSemaphoreGive(mtx);
    r->send(200, "application/json", ok ? "{\"ok\":true,\"set\":true}" : "{\"ok\":false,\"set\":false}");
  });

  // Sinkron paksa: timpa RTC + jam sistem dengan jam HP (hanya saat tidak ada sesi).
  server.on("/api/time/force", HTTP_POST, [](AsyncWebServerRequest* r) {
    long e = r->hasParam("epoch") ? r->getParam("epoch")->value().toInt() : 0;
    if (e <= 1700000000L) { sendErr(r, 400, "Epoch tidak valid"); return; }
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool busy = S.run || S.logging;          // jangan ubah jam saat sesi/log berjalan
    if (!busy) applyTime((uint32_t)e, 2);
    xSemaphoreGive(mtx);
    if (busy) sendErr(r, 409, "Sesi sedang berjalan");
    else r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- DAFTAR FILE LOG: nama, ukuran, timestamp baris terakhir ----
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* r) {
    String j; j.reserve(2048);
    xSemaphoreTake(mtx, portMAX_DELAY);
    j += "{\"sd\":\""; j += SDSTR[sdSt]; j += "\",\"active\":\""; j += logName; j += "\",\"files\":[";
    if (sdSt == 0) {
      File dir = SD.open(LOGS_DIR);
      if (dir) {
        int cnt = 0;
        File f = dir.openNextFile();
        while (f && cnt < MAX_LOG_FILES) {
          if (!f.isDirectory()) {
            String nm = f.name();
            int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
            if (nm.endsWith(".csv")) {
              nm.remove(nm.length() - 4);
              if (validName(nm)) {
                char last[24]; lastStamp(f, last, sizeof(last));
                if (cnt++) j += ',';
                j += "{\"name\":\""; j += nm; j += "\",\"size\":"; j += (unsigned)f.size();
                j += ",\"last\":\""; j += last; j += "\"}";
              }
            }
          }
          f.close();
          f = dir.openNextFile();
        }
        dir.close();
      }
    }
    xSemaphoreGive(mtx);
    j += "]}";
    r->send(200, "application/json", j);
  });

  // ---- INISIALISASI SESI LOG: mode=new (buat file baru) / mode=resume (lanjut file lama) ----
  server.on("/api/log/init", HTTP_POST, [](AsyncWebServerRequest* r) {
    String name = r->hasParam("name") ? r->getParam("name")->value() : "";
    bool resume = r->hasParam("mode") && r->getParam("mode")->value() == "resume";
    if (!validName(name)) { sendErr(r, 400, "Nama: huruf, angka, underscore (1-32)"); return; }
    String path = pathFor(name);
    const char* err = nullptr;
    int code = 409;
    bool exists = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (S.run || S.logging) err = "Sesi sedang berjalan";
    else if (sdSt != 0) { err = "SD card tidak terdeteksi"; code = 503; }
    else if (!timeSet) err = "Jam belum ter-set, muat ulang halaman";
    else {
      bool ex = SD.exists(path);
      if (resume) {
        if (!ex) { err = "File tidak ditemukan"; code = 404; }
        else { setActiveLog(name.c_str()); resumeSep = true; }
      } else if (ex) {
        err = "File sudah ada"; exists = true;
      } else {
        File f = SD.open(path, FILE_WRITE);
        if (!f) { err = "Gagal membuat file"; code = 500; }
        else {
          char ts[24]; nowStr(ts, sizeof(ts));
          f.print(LOG_HDR);
          f.printf("# Sesi dibuat %s\n", ts);
          f.close();
          setActiveLog(name.c_str());
          resumeSep = false;
        }
      }
    }
    xSemaphoreGive(mtx);
    if (err) {
      char b[160];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\",\"exists\":%s}", err, exists ? "true" : "false");
      r->send(code, "application/json", b);
    } else r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- BACA / DOWNLOAD SATU FILE LOG: /api/log?file=nama[&dl=1] ----
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (dlBusy) { sendErr(r, 503, "Download sedang berjalan, coba lagi"); return; }
    String name;
    if (r->hasParam("file")) name = r->getParam("file")->value();
    else { xSemaphoreTake(mtx, portMAX_DELAY); name = logName; xSemaphoreGive(mtx); }
    if (!validName(name)) { sendErr(r, 400, "Pilih file log dulu"); return; }
    String path = pathFor(name);
    size_t size = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    uint8_t st = sdSt;
    bool ex = fileInfo(path.c_str(), size);
    xSemaphoreGive(mtx);
    if (st != 0) { sendErr(r, 503, "SD card tidak terdeteksi"); return; }
    if (!ex) { sendErr(r, 404, "File tidak ditemukan"); return; }
    if (size == 0) { r->send(200, "text/csv", LOG_HDR); return; }
    dlBusy = true;
    r->onDisconnect([]() { dlBusy = false; });
    AsyncWebServerResponse* resp = r->beginResponse("text/csv", size,
      [size, path](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        if (index >= size) return 0;
        size_t want = size - index;
        if (want > maxLen) want = maxLen;
        if (want > 1024) want = 1024;
        size_t got = 0;
        xSemaphoreTake(mtx, portMAX_DELAY);
        File f = SD.open(path.c_str(), FILE_READ);
        if (f && f.seek(index)) got = f.read(buf, want);
        if (f) f.close();
        xSemaphoreGive(mtx);
        return got;
      });
    if (r->hasParam("dl")) {
      String cd = "attachment; filename=" + name + ".csv";
      resp->addHeader("Content-Disposition", cd);
    }
    r->send(resp);
  });

  // ---- HAPUS SATU FILE LOG: /api/log/delete?file=nama (belum ada tombol di UI) ----
  server.on("/api/log/delete", HTTP_POST, [](AsyncWebServerRequest* r) {
    String name = r->hasParam("file") ? r->getParam("file")->value() : "";
    if (!validName(name)) { sendErr(r, 400, "Nama file tidak valid"); return; }
    String path = pathFor(name);
    const char* err = nullptr;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (S.logging && strcasecmp(name.c_str(), logName) == 0) err = "File sedang dipakai logging";
    else if (dlBusy) err = "Download sedang berjalan";
    else if (sdSt != 0) err = "SD card tidak terdeteksi";
    else if (SD.exists(path)) {
      SD.remove(path);
      if (strcasecmp(name.c_str(), logName) == 0) clearActiveLog();
    }
    xSemaphoreGive(mtx);
    if (err) sendErr(r, 409, err);
    else r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/fs", HTTP_GET, [](AsyncWebServerRequest* r) {
    char buf[420];
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    xSemaphoreTake(mtx, portMAX_DELAY);
    int n = snprintf(buf, sizeof(buf),
      "{\"part_addr\":\"0x%x\",\"part_size\":%u,\"total\":%u,\"used\":%u,\"files\":\"",
      p ? (unsigned)p->address : 0, p ? (unsigned)p->size : 0,
      (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
    File root = LittleFS.open("/");
    File f = root ? root.openNextFile() : File();
    while (f && n < (int)sizeof(buf) - 40) {
      n += snprintf(buf + n, sizeof(buf) - n, "%s(%u) ", f.name(), (unsigned)f.size());
      f = root.openNextFile();
    }
    xSemaphoreGive(mtx);
    snprintf(buf + n, sizeof(buf) - n, "\"}");
    r->send(200, "application/json", buf);
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");
  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "Not found"); });
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);              // jangan menunggu bila laptop tidak terhubung (cegah web/tick tersendat)
#endif

  pinMode(RS485_CTRL, OUTPUT);
  digitalWrite(RS485_CTRL, LOW);

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  setenv("TZ", "WIB-7", 1); tzset();
  mtx = xSemaphoreCreateMutex();

  // ---- RTC: sumber waktu utama ----
  Wire.begin(RTC_SDA, RTC_SCL);
  rtcPresent = rtc.begin();
  if (rtcPresent) {
    rtcState = rtc.lostPower() ? 2 : 1;
    if (rtcState == 1) {
      uint32_t e = rtc.now().unixtime();
      if (e > 1700000000UL) applyTime(e, 1);   // waktu masuk akal -> pakai RTC
    }
  }
  Serial.printf("RTC: %s | sumber waktu: %s\n",
                rtcState == 0 ? "tidak terdeteksi" : (rtcState == 2 ? "ada, jam hilang" : "OK"),
                timeSrc == 1 ? "RTC" : "menunggu HP");

  if (!LittleFS.begin(false)) Serial.println("LittleFS GAGAL mount");
  else Serial.printf("LittleFS OK: total=%u used=%u\n",
                     (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
  if (LittleFS.exists(LEGACY_LOG)) LittleFS.remove(LEGACY_LOG);

  xSemaphoreTake(mtx, portMAX_DELAY);
  bool sdOk = sdMount();
  xSemaphoreGive(mtx);
  if (sdOk) Serial.printf("SD OK: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  else Serial.println("SD tidak terdeteksi");

  Serial1.begin(9600, SERIAL_8E1, RS485_RX, RS485_TX);
  node.begin(OMRON_ID, Serial1);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  xTaskCreate(modbusTask, "modbus", 4096, nullptr, 1, nullptr);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  setupRoutes();
  server.begin();
}

void loop() {
  static uint32_t lastTick = 0, lastHb = 0;
  uint32_t now = millis();
  if (now - lastTick >= 1000) { lastTick = now; tick(); emitSerialLog(); }
  if (now - lastHb >= 5000) {
    lastHb = now;
    xSemaphoreTake(mtx, portMAX_DELAY);
    Serial.printf("heap=%u klien=%d pv=%.1f sp_omron=%.1f mv=%.1f soak=%d lba=%s lbaOm=%s fase=%s sd=%s log=%s omron=%s modbus=0x%02X werr=0x%02X sensor=%s jam=%s | %s\n",
                  (unsigned)ESP.getFreeHeap(), WiFi.softAPgetStationNum(), S.pv, spOmron, mvPct,
                  soakRem == 0xFFFF ? -1 : (int)soakRem, lbaAlarm ? "ALARM" : "ok", lbaOmron ? "ALARM" : "ok",
                  PHASE[S.phase], SDSTR[sdSt], logName[0] ? logName : "-", commOk ? "ok" : "ERR", lastErr, lastWErr,
                  sensorOk ? "ok" : "ERR", TSTR[timeSrc], S.event);
    xSemaphoreGive(mtx);
  }
  delay(5);
}

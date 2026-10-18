#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <MHZ19.h>
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <freertos/semphr.h>
#include <memory>
#include <string.h>
#include <vector>

#define DHT_PIN            5
#define DHT_TYPE           22
#define MHZ_RX             17
#define MHZ_TX             16
#define LOGS_DIR           "/logs"
#define MAX_LOG_FILES      20
#define NTP_SERVER         "ru.pool.ntp.org"
#define MDNS_HOST          "climat"
#define DEFAULT_POINT_SEC  30
#define DEFAULT_SAVE_MIN   5
#define MIN_POINT_SEC      5
#define MAX_POINT_SEC      600
#define MIN_SAVE_MIN       1
#define MAX_SAVE_MIN       1440
#define MAX_LIVE_POINTS    1440
#define MAX_HISTORY_OUT    400
#define MAX_FILE_ROWS      1500
#define MHZ19_HEATING_TIME (1000UL * 60UL * 3UL)

const char *AP_SSID = "CO2Monitor";
const char *AP_PASS = "12345678";

#define DEFAULT_WIFI_SSID "Andrey_Lan"
#define DEFAULT_WIFI_PASS "2p0r1o8w"

DHT dht(DHT_PIN, DHT_TYPE);

HardwareSerial mhzSerial(1);
MHZ19          mhz;
Preferences    prefs;
AsyncWebServer server(80);
DNSServer      dnsServer;
bool           dnsStarted = false;

static SemaphoreHandle_t mhzMutex = nullptr;
static SemaphoreHandle_t fsMutex  = nullptr;

static bool mhzLock(uint32_t ms = 1000) {
  return mhzMutex && xSemaphoreTake(mhzMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void mhzUnlock() {
  if (mhzMutex)
    xSemaphoreGive(mhzMutex);
}

static bool fsLock(uint32_t ms = 2000) {
  return fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void fsUnlock() {
  if (fsMutex)
    xSemaphoreGive(fsMutex);
}

struct Reading {
  uint32_t timestamp   = 0;
  float    temperature = 0;
  float    humidity    = 0;
  uint16_t co2         = 0;
};

Reading  latest;
bool     dhtOk = false, mhzOk = false;
bool     mhzHeating      = true;
bool     mdnsStarted     = false;
bool     mdnsRestart     = false;
uint32_t heatingStartMs  = 0;
uint32_t lastLogMs       = 0;
uint32_t lastNtpTry      = 0;
uint32_t lastPointMs     = 0;
bool     apMode          = true;
bool     timeSynced      = false;
uint32_t pointIntervalMs = DEFAULT_POINT_SEC * 1000UL;
uint32_t saveIntervalMs  = DEFAULT_SAVE_MIN * 60UL * 1000UL;
uint32_t pointsSeq       = 0;
uint32_t logRowsTotal    = 0;
uint32_t oldestSavedTs   = 0;

struct Point {
  uint32_t ts;
  float    t;
  float    h;
  uint16_t co2;
};

Point               liveBuf[MAX_LIVE_POINTS];
int                 liveHead  = 0;
int                 liveCount = 0;
static portMUX_TYPE liveMux   = portMUX_INITIALIZER_UNLOCKED;

String cfgSsid, cfgPass;
int    mhzRange = 5000;

static void fmtTimeBuf(uint32_t ts, char *buf, size_t n) {
  struct tm t;
  time_t    tt = ts;
  localtime_r(&tt, &t);
  strftime(buf, n, "%Y-%m-%d %H:%M:%S", &t);
}

static String fmtTime(uint32_t ts) {
  char buf[24];
  fmtTimeBuf(ts, buf, sizeof(buf));
  return String(buf);
}

static String todayFile() {
  time_t    now = time(nullptr);
  struct tm t;
  if (timeSynced && now > 1000000) {
    localtime_r(&now, &t);
  } else {
    struct tm fallback;
    memset(&fallback, 0, sizeof(fallback));
    fallback.tm_year = 100;
    fallback.tm_mon  = 0;
    fallback.tm_mday = 1;
    t                = fallback;
  }
  char name[32];
  sprintf(name, "%s/%04d-%02d-%02d.csv", LOGS_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(name);
}

static void listFiles(const char *dirPath, std::vector<String> &out) {
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory())
    return;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    String name  = String(entry.name());
    int    slash = name.lastIndexOf('/');
    if (slash >= 0)
      name = name.substring(slash + 1);
    if (name.length())
      out.push_back(String(dirPath) + "/" + name);
    entry.close();
  }
  dir.close();
}

static uint32_t countFileRows(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f)
    return 0;
  uint32_t lines = 0;
  char     last  = '\n';
  uint8_t  buf[256];
  while (true) {
    size_t n = f.read(buf, sizeof(buf));
    if (!n)
      break;
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\n')
        lines++;
      last = (char)buf[i];
    }
  }
  f.close();
  if (!lines)
    return 0;
  if (last != '\n')
    lines++;
  return lines - 1;
}

static uint32_t firstFileTs(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f)
    return 0;
  f.readStringUntil('\n');
  String line = f.readStringUntil('\n');
  f.close();
  int comma = line.indexOf(',');
  if (comma <= 0)
    return 0;
  return (uint32_t)strtoul(line.substring(0, comma).c_str(), nullptr, 10);
}

static void recountLogRows() {
  std::vector<String> names;
  listFiles(LOGS_DIR, names);
  uint32_t total = 0;
  for (const String &n : names)
    total += countFileRows(n);
  logRowsTotal = total;

  oldestSavedTs = 0;
  if (!names.empty()) {
    int oi = 0;
    for (size_t i = 1; i < names.size(); i++)
      if (names[i] < names[oi])
        oi = i;
    oldestSavedTs = firstFileTs(names[oi]);
  }
  Serial.printf("[LOG] файлов %u, записей всего %lu, самая ранняя %lu\n",
                (unsigned)names.size(), (unsigned long)total,
                (unsigned long)oldestSavedTs);
}

static void rotateLogsIfNeeded() {
  std::vector<String> names;
  listFiles(LOGS_DIR, names);

  int removed = 0;
  while (names.size() > MAX_LOG_FILES && removed < 2) {
    int oldestIdx = 0;
    for (size_t i = 1; i < names.size(); i++)
      if (names[i] < names[oldestIdx])
        oldestIdx = i;
    Serial.printf("[LOG] ротация: удаляю %s\n", names[oldestIdx].c_str());
    uint32_t gone = countFileRows(names[oldestIdx]);
    logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
    LittleFS.remove(names[oldestIdx]);
    names.erase(names.begin() + oldestIdx);
    removed++;
    oldestSavedTs = 0;
    if (!names.empty()) {
      int oi = 0;
      for (size_t i = 1; i < names.size(); i++)
        if (names[i] < names[oi])
          oi = i;
      oldestSavedTs = firstFileTs(names[oi]);
    }
  }
}

static void logReading(const Reading &r) {
  if (!timeSynced || (time(nullptr) < 1000000)) {
    Serial.println("[LOG] время не синхронизировано (NTP), пропускаю запись в файл");
    return;
  }
  if (!fsLock(300)) {
    Serial.println("[LOG] ФС занята, запись пропущена");
    return;
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);
  String path  = todayFile();
  bool   isNew = !LittleFS.exists(path);

  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.println("[LOG] ошибка открытия файла");
    fsUnlock();
    return;
  }
  if (isNew)
    f.println("timestamp,datetime,temperature,humidity,co2");
  f.printf("%lu,%s,%.2f,%.2f,%u\n", r.timestamp, fmtTime(r.timestamp).c_str(),
           r.temperature, r.humidity, r.co2);
  f.close();
  logRowsTotal++;
  pointsSeq++;
  Serial.printf("[LOG] записано в %s: %.1fC %.1f%% %uppm\n",
                path.c_str(), r.temperature, r.humidity, r.co2);
  rotateLogsIfNeeded();
  fsUnlock();
}

static void pushLivePoint(const Reading &r) {
  portENTER_CRITICAL(&liveMux);
  Point &p = liveBuf[liveHead];
  p.ts     = r.timestamp;
  p.t      = r.temperature;
  p.h      = r.humidity;
  p.co2    = r.co2;
  liveHead = (liveHead + 1) % MAX_LIVE_POINTS;
  if (liveCount < MAX_LIVE_POINTS)
    liveCount++;
  portEXIT_CRITICAL(&liveMux);
  pointsSeq++;
}

static void liveSnapshot(int &head, int &count) {
  portENTER_CRITICAL(&liveMux);
  head  = liveHead;
  count = liveCount;
  portEXIT_CRITICAL(&liveMux);
}

static Point liveGet(int idx) {
  Point p;
  portENTER_CRITICAL(&liveMux);
  p = liveBuf[idx];
  portEXIT_CRITICAL(&liveMux);
  return p;
}

static portMUX_TYPE latestMux = portMUX_INITIALIZER_UNLOCKED;

static void setLatestTh(float t, float h) {
  portENTER_CRITICAL(&latestMux);
  latest.temperature = t;
  latest.humidity    = h;
  portEXIT_CRITICAL(&latestMux);
}

static void setLatestCo2(uint16_t c) {
  portENTER_CRITICAL(&latestMux);
  latest.co2 = c;
  portEXIT_CRITICAL(&latestMux);
}

static void setLatestTs(uint32_t ts) {
  portENTER_CRITICAL(&latestMux);
  latest.timestamp = ts;
  portEXIT_CRITICAL(&latestMux);
}

static Reading getLatest() {
  Reading r;
  portENTER_CRITICAL(&latestMux);
  r = latest;
  portEXIT_CRITICAL(&latestMux);
  return r;
}

static void readSensors() {
  float t  = dht.readTemperature();
  float h  = dht.readHumidity();
  bool  ok = !isnan(t) && !isnan(h);
  if (ok)
    setLatestTh(t, h);

  static int lastOk = -1;
  if ((int)ok != lastOk) {
    lastOk = ok;
    if (ok)
      Serial.printf("[DHT] pin=%d OK, t=%.2f h=%.2f\n", DHT_PIN, t, h);
    else
      Serial.printf("[DHT] pin=%d нет отклика (NaN) — проверь питание/пин/подтяжку\n", DHT_PIN);
  }
  dhtOk = ok;

  uint32_t uptime = millis() - heatingStartMs;
  mhzHeating      = (uptime < MHZ19_HEATING_TIME);
  if (mhzHeating) {
    static uint32_t lastHeatLog = 0xFFFFFFFFUL;
    if (uptime / 30000UL != lastHeatLog) {
      lastHeatLog = uptime / 30000UL;
      Serial.printf("[MHZ] прогрев: %lu/%lu с — показания не пишутся\n",
                    uptime / 1000UL, MHZ19_HEATING_TIME / 1000UL);
    }
    setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
    return;
  }

  static int      co2Fail   = 0;
  static byte     lastErr   = 0xFF;
  static uint32_t lastMhzMs = 0;

  if (co2Fail >= 3 && millis() - lastMhzMs < 30000) {
    setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
    return;
  }
  lastMhzMs = millis();

  if (!mhzLock()) {
    setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
    return;
  }
  int  co2 = mhz.getCO2(false, true);
  byte err = mhz.errorCode;
  mhzUnlock();
  bool co2ok = (co2 > 0 && co2 <= mhzRange);

  if (co2ok) {
    co2Fail = 0;
    setLatestCo2(co2);
    mhzOk = true;
  } else if (err == RESULT_FILTER) {
    co2Fail = 0;
  } else {
    if (co2Fail < 100)
      co2Fail++;
    if (co2Fail >= 3)
      mhzOk = false;
  }
  if (err != lastErr) {
    lastErr = err;
    if (co2ok)
      Serial.printf("[MHZ] OK: %d ppm (err=%u)\n", co2, err);
    else if (err == RESULT_FILTER)
      Serial.println("[MHZ] filter: датчик ответил, значение отбраковано (сброс ABC)");
    else
      Serial.printf("[MHZ] ошибка чтения, код %u (1=OK 2=timeout 3=match 4=crc 5=filter), сбоев подряд: %d\n",
                    err, co2Fail);
  }
  setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
}

static void tryNtp() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  unsigned long now = millis();

  if (now - lastNtpTry < 10000)
    return;
  lastNtpTry                 = now;
  static bool sntpConfigured = false;
  if (!sntpConfigured) {
    configTime(3 * 3600, 0, NTP_SERVER);
    sntpConfigured = true;
  }
  time_t tt = time(nullptr);
  if (tt > 1000000 && !timeSynced) {
    timeSynced = true;
    struct tm t;
    localtime_r(&tt, &t);
    Serial.printf("[NTP] Синхронизирован: %04d-%02d-%02d %02d:%02d:%02d (MSK)\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    setLatestTs((uint32_t)tt);
    if (!mhzHeating) {
      logReading(latest);
      pushLivePoint(latest);
    }
    lastLogMs   = millis();
    lastPointMs = millis();
  }
}

static uint64_t uptimeMsTotal() {
  static uint32_t last  = 0;
  static uint64_t total = 0;
  uint32_t        now   = millis();
  total += (uint32_t)(now - last);
  last = now;
  return total;
}

static size_t jsonCurrent(char *buf, size_t n) {
  JsonDocument doc;
  Reading      r     = getLatest();
  doc["temperature"] = r.temperature;
  doc["humidity"]    = r.humidity;
  doc["co2"]         = r.co2;
  doc["dhtOk"]       = dhtOk;
  doc["mhzOk"]       = mhzOk;
  doc["heating"]     = mhzHeating;
  doc["heatLeft"]    = mhzHeating
                           ? (int)((MHZ19_HEATING_TIME - (millis() - heatingStartMs)) / 1000UL)
                           : 0;
  doc["timestamp"]   = r.timestamp;
  bool timeOk        = (time(nullptr) > 1000000);
  doc["timeOk"]      = timeOk;
  if (timeOk) {
    doc["datetime"] = fmtTime(r.timestamp);
  } else {
    doc["datetime"] = "NTP ожидание...";
  }
  doc["uptime"]   = (uint32_t)(uptimeMsTotal() / 1000);
  doc["mode"]     = apMode ? "AP" : "STA";
  doc["ip"]       = WiFi.localIP().toString();
  doc["mdns"]     = String(MDNS_HOST) + ".local";
  doc["mdnsOk"]   = mdnsStarted;
  uint32_t nextIn = 0;
  if (timeSynced && time(nullptr) > 1000000) {
    uint32_t elapsed = millis() - lastLogMs;
    if (elapsed < saveIntervalMs)
      nextIn = (saveIntervalMs - elapsed) / 1000;
  }
  doc["nextLogInSec"] = nextIn;
  doc["pointSec"]     = pointIntervalMs / 1000;
  doc["saveMin"]      = saveIntervalMs / 60000;
  int lvHead, lvCount;
  liveSnapshot(lvHead, lvCount);
  doc["points"] = lvCount;

  doc["logRows"]   = logRowsTotal;
  uint32_t firstTs = oldestSavedTs;
  if (lvCount) {
    uint32_t ramFirst = liveGet((lvHead - lvCount + MAX_LIVE_POINTS) % MAX_LIVE_POINTS).ts;
    if (ramFirst && (!firstTs || ramFirst < firstTs))
      firstTs = ramFirst;
  }
  doc["firstTs"]   = firstTs;
  doc["pointsSeq"] = pointsSeq;
  return serializeJson(doc, buf, n);
}

struct HPoint {
  uint32_t ts;
  float    t;
  float    h;
  uint16_t co2;
};

static void readFileTail(const String &path, int maxRows, std::vector<HPoint> &out) {
  out.clear();
  if (maxRows < 1)
    return;
  File f = LittleFS.open(path, "r");
  if (!f)
    return;

  std::vector<HPoint> ring(maxRows);
  int                 cnt = 0, head = 0;
  char                line[128];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0)
      break;
    line[n] = 0;
    if (line[0] == 0 || strncmp(line, "timestamp", 9) == 0)
      continue;
    char *c1 = strchr(line, ',');
    if (!c1)
      continue;
    char *c2 = strchr(c1 + 1, ',');
    if (!c2)
      continue;
    char *c3 = strchr(c2 + 1, ',');
    if (!c3)
      continue;
    char *c4 = strchr(c3 + 1, ',');
    if (!c4)
      continue;
    uint32_t ts = (uint32_t)strtoul(line, nullptr, 10);
    if (ts == 0)
      continue;
    HPoint &p = ring[head];
    p.ts      = ts;
    p.t       = atof(c2 + 1);
    p.h       = atof(c3 + 1);
    p.co2     = (uint16_t)atoi(c4 + 1);
    head      = (head + 1) % maxRows;
    if (cnt < maxRows)
      cnt++;
  }
  f.close();

  int start = (head - cnt + maxRows) % maxRows;
  out.reserve(cnt);
  for (int i = 0; i < cnt; i++)
    out.push_back(ring[(start + i) % maxRows]);
}

static void fmtPoint(char *buf, size_t n, const HPoint &p) {
  char dt[24];
  fmtTimeBuf(p.ts, dt, sizeof(dt));
  snprintf(buf, n,
           "{\"ts\":%lu,\"dt\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"co2\":%u}",
           (unsigned long)p.ts, dt, p.t, p.h, p.co2);
}

static void printPoint(Print &out, const HPoint &p, bool &first) {
  if (!first)
    out.print(',');
  first = false;
  char buf[160];
  fmtPoint(buf, sizeof(buf), p);
  out.print(buf);
}

static void sendHistorySince(AsyncWebServerRequest *req, uint32_t since) {
  AsyncResponseStream *resp = req->beginResponseStream("application/json");
  resp->print('[');
  bool first = true;
  int  lh, lc;
  liveSnapshot(lh, lc);
  int start = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;
  int fresh = 0;
  for (int i = 0; i < lc; i++) {
    if (liveGet((start + i) % MAX_LIVE_POINTS).ts > since)
      fresh++;
  }
  int skip = (fresh > MAX_HISTORY_OUT) ? fresh - MAX_HISTORY_OUT : 0;
  for (int i = 0, seen = 0; i < lc; i++) {
    Point sp = liveGet((start + i) % MAX_LIVE_POINTS);
    if (sp.ts <= since)
      continue;
    if (seen++ < skip)
      continue;
    printPoint(*resp, HPoint{sp.ts, sp.t, sp.h, sp.co2}, first);
  }
  resp->print(']');
  req->send(resp);
}

static void sendHistory(AsyncWebServerRequest *req, uint32_t since, int lastN) {
  int maxOut = (lastN > 0) ? lastN : MAX_HISTORY_OUT;

  int lh, lc;
  liveSnapshot(lh, lc);
  int start = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;

  int      firstPass  = 0;
  uint32_t oldestLive = 0xFFFFFFFFu;
  while (firstPass < lc) {
    uint32_t ts = liveGet((start + firstPass) % MAX_LIVE_POINTS).ts;
    if (ts != 0 && !(since && ts < since)) {
      oldestLive = ts;
      break;
    }
    firstPass++;
  }
  int      liveAfter  = lc - firstPass;
  uint32_t liveNewest = lc ? liveGet((start + lc - 1) % MAX_LIVE_POINTS).ts : 0;

  int need;
  if (lastN > 0) {
    need = lastN - liveAfter;
  } else if (since && oldestLive != 0xFFFFFFFFu) {
    uint32_t saveSec = saveIntervalMs / 1000UL;
    if (saveSec < 60)
      saveSec = 60;
    uint32_t sec = (oldestLive > since) ? (oldestLive - since) : 0;
    need         = (int)(sec / saveSec) + 2;
  } else {
    need = MAX_FILE_ROWS;
  }
  if (need < 0)
    need = 0;
  if (need > MAX_FILE_ROWS)
    need = MAX_FILE_ROWS;
  std::vector<HPoint> rows;
  if (need > 0 && fsLock()) {
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);

    for (int fi = (int)names.size() - 1; fi >= 0 && (int)rows.size() < need; fi--) {
      int perFile = need - (int)rows.size();
      if (perFile > 600)
        perFile = 600;
      std::vector<HPoint> tail;
      readFileTail(names[fi], perFile, tail);
      if (tail.empty())
        continue;
      if (since && tail.back().ts < since)
        break;
      std::vector<HPoint> keep;
      keep.reserve(tail.size());
      for (const HPoint &p : tail) {
        if (p.ts == 0 || p.ts >= oldestLive)
          continue;
        if (since && p.ts < since)
          continue;
        keep.push_back(p);
      }
      rows.insert(rows.begin(), keep.begin(), keep.end());
    }
    fsUnlock();
  }

  int liveKeep = liveAfter;
  int minLive  = maxOut / 3;
  int room     = maxOut - (int)rows.size();
  if (room < minLive)
    room = minLive;
  if (liveKeep > room)
    liveKeep = room;
  int fileBudget = maxOut - liveKeep;
  if (fileBudget < 1)
    fileBudget = 1;

  std::vector<HPoint> outv;
  outv.reserve(maxOut);
  if (!rows.empty()) {
    if ((int)rows.size() <= fileBudget) {
      outv.insert(outv.end(), rows.begin(), rows.end());
    } else {
      int step = (int)((rows.size() + fileBudget - 1) / fileBudget);
      int off  = ((int)rows.size() - 1) % step;
      for (int i = off; i < (int)rows.size(); i += step)
        outv.push_back(rows[i]);
    }
  }
  if (liveAfter > 0) {
    int step = (liveAfter + liveKeep - 1) / liveKeep;
    if (step < 1)
      step = 1;
    int off = (liveAfter - 1) % step;
    for (int k = off; k < liveAfter; k += step) {
      Point sp = liveGet((start + firstPass + k) % MAX_LIVE_POINTS);
      if (sp.ts == 0 || sp.ts < oldestLive || sp.ts > liveNewest)
        continue;
      outv.push_back(HPoint{sp.ts, sp.t, sp.h, sp.co2});
    }
  }

  AsyncResponseStream *resp = req->beginResponseStream("application/json");
  resp->print('[');
  bool firstItem = true;
  for (const HPoint &p : outv)
    printPoint(*resp, p, firstItem);
  resp->print(']');
  req->send(resp);
}

static String listLogFiles() {
  String              out = "[";
  std::vector<String> names;
  listFiles(LOGS_DIR, names);

  for (size_t i = 0; i < names.size(); i++)
    for (size_t j = i + 1; j < names.size(); j++)
      if (names[j] < names[i])
        std::swap(names[i], names[j]);
  for (size_t i = 0; i < names.size(); i++) {
    int    slash = names[i].lastIndexOf('/');
    String base  = slash >= 0 ? names[i].substring(slash + 1) : names[i];
    if (i)
      out += ",";
    out += "\"" + base + "\"";
  }
  out += "]";
  return out;
}

extern const uint8_t        index_html_start[] asm("_binary_src_index_html_start");
extern const uint8_t        index_html_end[] asm("_binary_src_index_html_end");
static const uint8_t *const INDEX_HTML     = index_html_start;
static const size_t         INDEX_HTML_LEN = (size_t)(index_html_end - index_html_start);

struct ExportCtx {
  std::vector<String> names;
  size_t              idx = 0;
  File                f;
  size_t              headerPos = 0;
  char                pending[168];
  size_t              pendingLen = 0;
};

static bool exportNextLine(ExportCtx &c, char *line, size_t n) {
  while (c.idx < c.names.size()) {
    if (!c.f) {
      c.f = LittleFS.open(c.names[c.idx], "r");
      if (!c.f) {
        c.idx++;
        continue;
      }
    }
    if (!c.f.available()) {
      c.f.close();
      c.idx++;
      continue;
    }
    size_t ln = c.f.readBytesUntil('\n', line, n - 1);
    if (ln == 0) {
      c.f.close();
      c.idx++;
      continue;
    }
    line[ln] = 0;
    if (line[0] == 0 || strncmp(line, "timestamp", 9) == 0)
      continue;
    return true;
  }
  return false;
}

static size_t exportFill(void *ctx, uint8_t *buf, size_t maxLen) {
  ExportCtx        *c   = (ExportCtx *)ctx;
  size_t            out = 0;
  static const char H[] = "timestamp,datetime,temperature,humidity,co2\n";
  const size_t      HL  = sizeof(H) - 1;

  if (maxLen == 0) {
    if (c->headerPos < HL || c->pendingLen)
      return RESPONSE_TRY_AGAIN;
    char peek[160];
    if (!exportNextLine(*c, peek, sizeof(peek)))
      return 0;
    size_t pn = strlen(peek);
    memcpy(c->pending, peek, pn);
    c->pending[pn] = '\n';
    c->pendingLen  = pn + 1;
    return RESPONSE_TRY_AGAIN;
  }

  bool locked = fsLock(2000);
  char line[160];

  while (out < maxLen) {
    if (c->headerPos < HL) {
      size_t take = HL - c->headerPos;
      if (take > maxLen - out)
        take = maxLen - out;
      memcpy(buf + out, H + c->headerPos, take);
      out += take;
      c->headerPos += take;
      continue;
    }
    if (c->pendingLen) {
      size_t take = c->pendingLen;
      if (take > maxLen - out)
        take = maxLen - out;
      memcpy(buf + out, c->pending, take);
      out += take;
      if (take < c->pendingLen) {
        memmove(c->pending, c->pending + take, c->pendingLen - take);
        c->pendingLen -= take;
        break;
      }
      c->pendingLen = 0;
      continue;
    }
    if (!exportNextLine(*c, line, sizeof(line)))
      break;
    size_t ll   = strlen(line);
    size_t take = ll;
    if (take > maxLen - out)
      take = maxLen - out;
    memcpy(buf + out, line, take);
    out += take;
    if (take < ll) {
      memcpy(c->pending, line + take, ll - take);
      c->pending[ll - take] = '\n';
      c->pendingLen         = ll - take + 1;
      break;
    }
    if (out < maxLen) {
      buf[out++] = '\n';
    } else {
      c->pending[0] = '\n';
      c->pendingLen = 1;
      break;
    }
  }

  if (locked)
    fsUnlock();
  return out;
}

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(new AsyncProgmemResponse(200, "text/html", INDEX_HTML, INDEX_HTML_LEN));
  });

  server.on("/api/current", HTTP_GET, [](AsyncWebServerRequest *req) {
    char buf[768];
    jsonCurrent(buf, sizeof(buf));
    req->send(200, "application/json", buf);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("since")) {
      uint32_t since = (uint32_t)strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
      sendHistorySince(req, since);
      return;
    }

    if (req->hasParam("points")) {
      int pts = (int)req->getParam("points")->value().toInt();
      pts     = constrain(pts, 10, MAX_HISTORY_OUT);
      sendHistory(req, 0, pts);
      return;
    }

    int hours = 12;
    if (req->hasParam("hours"))
      hours = req->getParam("hours")->value().toInt();
    uint32_t since = 0;
    if (hours > 0 && time(nullptr) > 1000000)
      since = (uint32_t)time(nullptr) - (uint32_t)hours * 3600UL;
    Serial.printf("[MEM] /api/history: период %d ч (since=%lu), heap=%u\n",
                  hours, (unsigned long)since, (unsigned)ESP.getFreeHeap());
    sendHistory(req, since, 0);
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["pointSec"]    = pointIntervalMs / 1000;
    doc["saveMin"]     = saveIntervalMs / 60000;
    doc["minPointSec"] = MIN_POINT_SEC;
    doc["maxPointSec"] = MAX_POINT_SEC;
    doc["minSaveMin"]  = MIN_SAVE_MIN;
    doc["maxSaveMin"]  = MAX_SAVE_MIN;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("pointSec", true)) {
      int v           = req->getParam("pointSec", true)->value().toInt();
      v               = constrain(v, MIN_POINT_SEC, MAX_POINT_SEC);
      pointIntervalMs = (uint32_t)v * 1000UL;
      prefs.putUInt("psec", (uint32_t)v);
    }
    if (req->hasParam("saveMin", true)) {
      int v          = req->getParam("saveMin", true)->value().toInt();
      v              = constrain(v, MIN_SAVE_MIN, MAX_SAVE_MIN);
      saveIntervalMs = (uint32_t)v * 60000UL;
      prefs.putUInt("smin", (uint32_t)v);
    }
    Serial.printf("[CFG] период точки: %lu с, сохранение: %lu мин\n",
                  pointIntervalMs / 1000, saveIntervalMs / 60000);
    req->send(200, "text/plain", "Сохранено");
  });

  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *req) {
    String out = "[]";
    if (fsLock(500)) {
      out = listLogFiles();
      fsUnlock();
    }
    req->send(200, "application/json", out);
  });

  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *req) {
    std::vector<String> names;
    if (fsLock(2000)) {
      listFiles(LOGS_DIR, names);
      fsUnlock();
    }
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);
    auto ctx   = std::make_shared<ExportCtx>();
    ctx->names = std::move(names);
    req->sendChunked("text/csv", [ctx](uint8_t *buf, size_t maxLen, size_t) -> size_t {
      return exportFill(ctx.get(), buf, maxLen);
    });
  });

  server.on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    String ssid, pass;
    if (req->hasParam("ssid", true))
      ssid = req->getParam("ssid", true)->value();
    if (req->hasParam("pass", true))
      pass = req->getParam("pass", true)->value();
    if (ssid.length()) {
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.putBool("noauto", false);
      req->send(200, "text/plain", "Сохранено. Перезагрузка в STA-режим...");
      delay(1500);
      ESP.restart();
    } else {
      req->send(400, "text/plain", "Укажите SSID");
    }
  });

  server.on("/api/wifi/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    prefs.clear();
    prefs.putBool("noauto", true);
    req->send(200, "text/plain", "Настройки стёрты. Возврат в AP...");
    delay(1500);
    ESP.restart();
  });

  server.on("/api/calibrate", HTTP_GET, [](AsyncWebServerRequest *req) {
    String type = "zero";
    if (req->hasParam("type"))
      type = req->getParam("type")->value();

    String resp;
    if (!mhzLock(2000)) {
      resp = "Датчик занят, повторите запрос";
    } else {
      if (type == "zero") {
        mhz.calibrate();
        resp = "Команда Cal Zero (400 ppm) отправлена. Не трогайте датчик ~1 минуты.";
      } else if (type == "span") {
        mhz.zeroSpan(2000);
        resp = "Команда Cal Span (2000 ppm) отправлена.";
      } else if (type == "abc_on") {
        mhz.autoCalibration(true, 24);
        prefs.putBool("abc", true);
        resp = "Автоматическая базовая калибровка (ABC, раз в 24 ч) включена.";
      } else if (type == "abc_off") {
        mhz.autoCalibration(false);
        prefs.putBool("abc", false);
        resp = "Автоматическая базовая калибровка (ABC) выключена.";
      } else {
        resp = "Неизвестная команда";
      }
      mhzUnlock();
    }
    Serial.println("[CAL] " + resp);
    req->send(200, "text/plain", resp);
  });

  server.on("/api/sensor", HTTP_GET, [](AsyncWebServerRequest *req) {
    int  range = mhzRange;
    bool abc   = prefs.getBool("abc", true);
    if (mhzLock()) {
      range = mhz.getRange();
      abc   = mhz.getABC();
      mhzUnlock();
    }
    JsonDocument doc;
    doc["range"]   = range;
    doc["abc"]     = abc;
    doc["co2"]     = getLatest().co2;
    doc["mhzOk"]   = mhzOk;
    doc["heating"] = mhzHeating;
    char out[192];
    serializeJson(doc, out, sizeof(out));
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!fsLock()) {
      req->send(503, "text/plain", "Файловая система занята, повторите");
      return;
    }
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);
    const int perPass = 2;
    int       removed = 0;
    for (auto &n : names) {
      if (removed >= perPass)
        break;
      uint32_t gone = countFileRows(n);
      logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
      LittleFS.remove(n);
      removed++;
    }
    int left      = (int)names.size() - removed;
    oldestSavedTs = (left > 0) ? firstFileTs(names[removed]) : 0;
    fsUnlock();
    if (left > 0) {
      req->send(200, "text/plain",
                "Удалено " + String(removed) + ", осталось " + String(left));
      return;
    }
    req->send(200, "text/plain", "Логи удалены");
  });

  server.begin();
}

static uint32_t lastMdnsStaIp = 0;

static void startMdns() {
  if (mdnsStarted && !mdnsRestart)
    return;

  static uint32_t lastTry = 0;
  if (!mdnsRestart && lastTry && millis() - lastTry < 10000)
    return;
  lastTry = millis();

  if (mdnsRestart) {
    mdnsRestart = false;
    if (mdnsStarted && WiFi.status() == WL_CONNECTED &&
        lastMdnsStaIp != 0 && (uint32_t)WiFi.localIP() == lastMdnsStaIp)
      return;
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }
  }

  if (WiFi.getMode() == WIFI_OFF)
    return;

  if (!MDNS.begin(MDNS_HOST)) {
    Serial.println("[MDNS] не запустился, повторю позже");
    return;
  }
  MDNS.setInstanceName("Climat Monitor");
  MDNS.addService("http", "tcp", 80);
  mdnsStarted   = true;
  lastMdnsStaIp = (WiFi.status() == WL_CONNECTED) ? (uint32_t)WiFi.localIP() : 0;
  Serial.printf("[MDNS] открыть: http://%s.local (%s, IP %s)\n", MDNS_HOST,
                apMode ? "AP" : "STA",
                apMode ? WiFi.softAPIP().toString().c_str()
                       : WiFi.localIP().toString().c_str());
}

static void setupNetwork() {
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");

  if (cfgSsid.length() == 0 && !prefs.getBool("noauto", false)) {
    cfgSsid = DEFAULT_WIFI_SSID;
    cfgPass = DEFAULT_WIFI_PASS;
    Serial.printf("[NET] сохранённой сети нет — пробую сеть по умолчанию: %s\n",
                  DEFAULT_WIFI_SSID);
  }

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[NET] STA подключено, IP: %s\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      mdnsRestart = true;
    }
  });

  if (cfgSsid.length()) {
    Serial.println("[NET] Пробую подключиться к: " + cfgSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setHostname(MDNS_HOST);
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
      delay(200);
    apMode = (WiFi.status() != WL_CONNECTED);
    if (apMode)
      Serial.println("[NET] Не удалось, работаю в режиме точки доступа");
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
  }

  lastNtpTry = 0;
  tryNtp();

  dnsServer.start(53, "*", WiFi.softAPIP());
  dnsStarted = true;
  startMdns();
  if (apMode) {
    Serial.printf("[NET] AP: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Climat Monitor ===");

  mhzMutex = xSemaphoreCreateMutex();
  fsMutex  = xSemaphoreCreateMutex();

  prefs.begin("cfg", false);

  {
    uint32_t ps = prefs.getUInt("psec", DEFAULT_POINT_SEC);
    uint32_t sm = prefs.getUInt("smin", DEFAULT_SAVE_MIN);
    if (ps < MIN_POINT_SEC || ps > MAX_POINT_SEC)
      ps = DEFAULT_POINT_SEC;
    if (sm < MIN_SAVE_MIN || sm > MAX_SAVE_MIN)
      sm = DEFAULT_SAVE_MIN;
    pointIntervalMs = ps * 1000UL;
    saveIntervalMs  = sm * 60000UL;
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS отформатирован");
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  recountLogRows();
  dht.begin();

  mhzSerial.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
  heatingStartMs = millis();
  mhz.begin(mhzSerial);
  mhz.setFilter(true, false);
  mhz.setRange(5000);
  mhz.autoCalibration(prefs.getBool("abc", true), 24);
  mhzRange = mhz.getRange();
  Serial.printf("[MHZ] UART1 rx=%d tx=%d, range=%d, ABC=%d, err=%u, прогрев %lu с\n",
                MHZ_RX, MHZ_TX, mhzRange,
                prefs.getBool("abc", true) ? 1 : 0, mhz.errorCode,
                MHZ19_HEATING_TIME / 1000UL);
  delay(3000);

  setupNetwork();
  setupWeb();

  readSensors();
  setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
  lastLogMs   = millis();
  lastPointMs = millis();
  Serial.printf("[INIT] точка каждые %lu с, сохранение в файл каждые %lu мин\n",
                pointIntervalMs / 1000, saveIntervalMs / 60000);
  Serial.println("[INIT] готово");
}

void loop() {
  if (dnsStarted)
    dnsServer.processNextRequest();

  startMdns();
  static uint32_t lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();
    readSensors();
    tryNtp();
  }

  if (timeSynced && millis() - lastPointMs >= pointIntervalMs) {
    lastPointMs = millis();
    setLatestTs((uint32_t)time(nullptr));
    if ((dhtOk || mhzOk) && !mhzHeating)
      pushLivePoint(latest);
  }

  if (millis() - lastLogMs >= saveIntervalMs) {
    lastLogMs = millis();
    setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
    if ((dhtOk || mhzOk) && !mhzHeating)
      logReading(latest);
  }

  delay(50);
}

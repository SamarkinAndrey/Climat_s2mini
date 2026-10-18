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
#include <vector>

#define DHT_PIN       5
#define DHT_TYPE      22
#define MHZ_RX        17
#define MHZ_TX        16
#define LOGS_DIR      "/logs"
#define MAX_LOG_FILES 20
#define NTP_SERVER    "ru.pool.ntp.org"

// Преднагрев MH-Z19: первые минуты после включения датчик отдаёт мусор
// (обычно ~3 минуты). В это время показания не читаем, не пишем и не помним.
#define MHZ19_HEATING_TIME (1000UL * 60UL * 3UL)

// Имя устройства в сети (mDNS): открывать как http://climat.local
#define MDNS_HOST "climat"

#define DEFAULT_POINT_SEC 30
#define DEFAULT_SAVE_MIN  5
#define MIN_POINT_SEC     5
#define MAX_POINT_SEC     600
#define MIN_SAVE_MIN      1
#define MAX_SAVE_MIN      1440
#define MAX_LIVE_POINTS   1440
// Предел точек в одном ответе истории. Больше не нужно: график на телефоне
// ~360 px шириной, 400 точек уже плотнее пикселя, а отрисовка заметно медленнее.
#define MAX_HISTORY_OUT   400
// Сколько строк вообще читаем из файлов (16 байт на точку). Нужно, чтобы
// длинный период (5 суток) покрывался целиком, а прореживание шло уже потом.
#define MAX_FILE_ROWS     1500

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

struct Reading {
  uint32_t timestamp   = 0;
  float    temperature = 0;
  float    humidity    = 0;
  uint16_t co2         = 0;
};

Reading  latest;
bool     dhtOk = false, mhzOk = false;
bool     mhzHeating      = true;   // идёт преднагрев MH-Z19
bool     mdnsStarted     = false;  // mDNS запущен (http://climat.local)
bool     mdnsRestart     = false;  // сеть переподключилась — mDNS надо перерегистрировать
uint32_t heatingStartMs  = 0;      // момент старта датчика (для отсчёта прогрева)
uint32_t lastLogMs       = 0;
uint32_t lastNtpTry      = 0;
uint32_t lastPointMs     = 0;
bool     apMode          = true;
bool     timeSynced      = false;
uint32_t pointIntervalMs = DEFAULT_POINT_SEC * 1000UL;
uint32_t saveIntervalMs  = DEFAULT_SAVE_MIN * 60UL * 1000UL;
uint32_t pointsSeq       = 0;
// Всего записей в лог-файлах (без заголовков): считается один раз на старте
// и дальше только инкрементируется — читать файлы на каждый запрос не нужно.
uint32_t logRowsTotal    = 0;
// Время самой ранней сохранённой записи (для подсчёта покрытого периода).
uint32_t oldestSavedTs   = 0;

struct Point {
  uint32_t ts;
  float    t;
  float    h;
  uint16_t co2;
};

Point liveBuf[MAX_LIVE_POINTS];
int   liveHead  = 0;
int   liveCount = 0;

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

// Число строк данных в лог-файле (заголовок не считаем). Читаем блоками, не
// парсим содержимое — важно только количество переводов строк.
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
    lines++;           // файл без перевода строки в конце
  return lines - 1;    // минус строка-заголовок
}

// Unix-время первой (самой ранней) записи в файле — нужна для бейджа
// «Записей», где часы считаются от самой первой сохранённой точки.
static uint32_t firstFileTs(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f)
    return 0;
  f.readStringUntil('\n');              // заголовок
  String line = f.readStringUntil('\n'); // первая строка данных
  f.close();
  int comma = line.indexOf(',');
  if (comma <= 0)
    return 0;
  return (uint32_t)strtoul(line.substring(0, comma).c_str(), nullptr, 10);
}

static void rotateLogsIfNeeded() {
  std::vector<String> names;
  listFiles(LOGS_DIR, names);

  while (names.size() > MAX_LOG_FILES) {
    int oldestIdx = 0;
    for (size_t i = 1; i < names.size(); i++)
      if (names[i] < names[oldestIdx])
        oldestIdx = i;
    Serial.printf("[LOG] ротация: удаляю %s\n", names[oldestIdx].c_str());
    uint32_t gone = countFileRows(names[oldestIdx]);
    logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
    LittleFS.remove(names[oldestIdx]);
    names.erase(names.begin() + oldestIdx);
    // самая ранняя запись теперь в самом старом из оставшихся файлов
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

// Пересчёт числа записей по всем лог-файлам — только на старте (одно чтение
// файлов), дальше счётчик ведётся инкрементально.
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
        oi = i;                          // файлы названы по дате, свежие — в конце
    oldestSavedTs = firstFileTs(names[oi]);
  }
  Serial.printf("[LOG] файлов %u, записей всего %lu, самая ранняя %lu\n",
                (unsigned)names.size(), (unsigned long)total,
                (unsigned long)oldestSavedTs);
}

static void logReading(const Reading &r) {
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);
  if (!timeSynced || (time(nullptr) < 1000000)) {
    Serial.println("[LOG] время не синхронизировано (NTP), пропускаю запись в файл");
    return;
  }
  String path  = todayFile();
  bool   isNew = !LittleFS.exists(path);

  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.println("[LOG] ошибка открытия файла");
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
}

static void pushLivePoint(const Reading &r) {
  Point &p = liveBuf[liveHead];
  p.ts     = r.timestamp;
  p.t      = r.temperature;
  p.h      = r.humidity;
  p.co2    = r.co2;
  liveHead = (liveHead + 1) % MAX_LIVE_POINTS;
  if (liveCount < MAX_LIVE_POINTS)
    liveCount++;
  pointsSeq++;
}

static void readSensors() {
  float t  = dht.readTemperature();
  float h  = dht.readHumidity();
  bool  ok = !isnan(t) && !isnan(h);
  if (ok) {
    latest.temperature = t;
    latest.humidity    = h;
  }

  static int lastOk = -1;
  if ((int)ok != lastOk) {
    lastOk = ok;
    if (ok)
      Serial.printf("[DHT] pin=%d OK, t=%.2f h=%.2f\n", DHT_PIN, t, h);
    else
      Serial.printf("[DHT] pin=%d нет отклика (NaN) — проверь питание/пин/подтяжку\n", DHT_PIN);
  }
  dhtOk = ok;

  // --- MH-Z19 ---
  // Первые MHZ19_HEATING_TIME после включения датчик греется и отдаёт мусор:
  // не читаем его, не помечаем как «нет связи», точки в это время не пишутся.
  uint32_t uptime = millis() - heatingStartMs;
  mhzHeating      = (uptime < MHZ19_HEATING_TIME);
  if (mhzHeating) {
    static uint32_t lastHeatLog = 0xFFFFFFFFUL;
    if (uptime / 30000UL != lastHeatLog) {   // отмечаемся в мониторе раз в 30 с
      lastHeatLog = uptime / 30000UL;
      Serial.printf("[MHZ] прогрев: %lu/%lu с — показания не пишутся\n",
                    uptime / 1000UL, MHZ19_HEATING_TIME / 1000UL);
    }
    latest.timestamp = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;
    return;
  }

  int  co2   = mhz.getCO2(false, true);
  byte err   = mhz.errorCode;
  bool co2ok = (co2 > 0 && co2 <= mhzRange);

  static int  co2Fail = 0;
  static byte lastErr = 0xFF;
  if (co2ok) {
    co2Fail    = 0;
    latest.co2 = co2;
    mhzOk      = true;
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
  latest.timestamp = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;
}

static void tryNtp() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  unsigned long now = millis();

  if (now - lastNtpTry < 10000)
    return;
  lastNtpTry = now;
  configTime(3 * 3600, 0, NTP_SERVER);
  time_t tt = time(nullptr);
  if (tt > 1000000 && !timeSynced) {
    timeSynced = true;
    struct tm t;
    localtime_r(&tt, &t);
    Serial.printf("[NTP] Синхронизирован: %04d-%02d-%02d %02d:%02d:%02d (MSK)\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    latest.timestamp = (uint32_t)tt;
    if (!mhzHeating) {            // во время прогрева точки не создаём
      logReading(latest);
      pushLivePoint(latest);
    }
    lastLogMs   = millis();
    lastPointMs = millis();
  }
}

// millis() переполняется через 49,7 суток, поэтому время работы накапливаем
// отдельно: unsigned-разность корректно переживает переполнение счётчика.
static uint64_t uptimeMsTotal() {
  static uint32_t last  = 0;
  static uint64_t total = 0;
  uint32_t        now   = millis();
  total += (uint32_t)(now - last);
  last = now;
  return total;
}

static String jsonCurrent() {
  JsonDocument doc;
  doc["temperature"] = latest.temperature;
  doc["humidity"]    = latest.humidity;
  doc["co2"]         = latest.co2;
  doc["dhtOk"]       = dhtOk;
  doc["mhzOk"]       = mhzOk;
  doc["heating"]     = mhzHeating;
  doc["heatLeft"]    = mhzHeating
                         ? (int)((MHZ19_HEATING_TIME - (millis() - heatingStartMs)) / 1000UL)
                         : 0;
  doc["timestamp"]   = latest.timestamp;
  bool timeOk        = (time(nullptr) > 1000000);
  doc["timeOk"]      = timeOk;
  if (timeOk) {
    doc["datetime"] = fmtTime(latest.timestamp);
  } else {
    doc["datetime"] = "NTP ожидание...";
  }
  doc["uptime"]       = (uint32_t)(uptimeMsTotal() / 1000);
  doc["mode"]         = apMode ? "AP" : "STA";
  doc["ip"]           = WiFi.localIP().toString();
  doc["mdns"]         = String(MDNS_HOST) + ".local";
  doc["mdnsOk"]       = mdnsStarted;
  doc["nextLogInSec"] = (timeSynced && time(nullptr) > 1000000)
                            ? ((lastLogMs + saveIntervalMs > millis())
                                   ? (lastLogMs + saveIntervalMs - millis()) / 1000
                                   : 0)
                            : 0;
  doc["pointSec"]     = pointIntervalMs / 1000;
  doc["saveMin"]      = saveIntervalMs / 60000;
  doc["points"]       = liveCount;
  doc["logRows"]      = logRowsTotal;   // всего записей в файлах
  // Самая ранняя из сохранённых точек: файлы (если есть) либо начало ОЗУ.
  uint32_t firstTs = oldestSavedTs;
  if (liveCount) {
    uint32_t ramFirst = liveBuf[(liveHead - liveCount + MAX_LIVE_POINTS) % MAX_LIVE_POINTS].ts;
    if (ramFirst && (!firstTs || ramFirst < firstTs))
      firstTs = ramFirst;
  }
  doc["firstTs"]      = firstTs;
  doc["pointsSeq"]    = pointsSeq;
  String out;
  serializeJson(doc, out);
  return out;
}

// --- История: компактная точка (файловые записи тоже храним так, а не строками) ---
struct HPoint {
  uint32_t ts;
  float    t;
  float    h;
  uint16_t co2;
};

// Читает из файла ТОЛЬКО последние maxRows строк.
// Раньше файл целиком загружался в вектор String — на больших файлах это
// съедало всю память и устройство перезагружалось.
static void readFileTail(const String &path, int maxRows, std::vector<HPoint> &out) {
  out.clear();
  if (maxRows < 1)
    return;
  File f = LittleFS.open(path, "r");
  if (!f)
    return;

  std::vector<HPoint> ring(maxRows);
  int                 cnt = 0, head = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int    p1   = line.indexOf(',');
    if (p1 <= 0 || line.startsWith("timestamp"))
      continue;
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    if (p2 < 0 || p3 < 0 || p4 < 0)
      continue;
    uint32_t ts = (uint32_t)line.substring(0, p1).toInt();
    if (ts == 0)
      continue;
    HPoint &p = ring[head];
    p.ts      = ts;
    p.t       = line.substring(p2 + 1, p3).toFloat();
    p.h       = line.substring(p3 + 1, p4).toFloat();
    p.co2     = (uint16_t)line.substring(p4 + 1).toInt();
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

// Догрузка только новых точек (для живого обновления графиков).
// Отвечает из ОЗУ, файлы не читает — ответ крошечный.
static String jsonHistorySince(uint32_t since) {
  String out   = "[";
  bool   first = true;
  int    start = (liveHead - liveCount + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;
  for (int i = 0; i < liveCount; i++) {
    const Point &sp = liveBuf[(start + i) % MAX_LIVE_POINTS];
    if (sp.ts <= since)
      continue;
    HPoint p{sp.ts, sp.t, sp.h, sp.co2};
    if (!first)
      out += ',';
    first = false;
    char buf[160];
    fmtPoint(buf, sizeof(buf), p);
    out += buf;
  }
  out += "]";
  return out;
}

// since=0 — без ограничения по времени; lastN>0 — «последние N точек» (для таблицы).
static void sendHistory(AsyncWebServerRequest *req, uint32_t since, int lastN) {
  int maxOut = (lastN > 0) ? lastN : MAX_HISTORY_OUT;

  // --- точки из ОЗУ (высокое разрешение) ---
  std::vector<HPoint> live;
  live.reserve(liveCount);
  int first = (liveHead - liveCount + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;
  for (int i = 0; i < liveCount; i++) {
    const Point &sp = liveBuf[(first + i) % MAX_LIVE_POINTS];
    if (sp.ts == 0 || (since && sp.ts < since))
      continue;
    live.push_back(HPoint{sp.ts, sp.t, sp.h, sp.co2});
  }
  uint32_t oldestLive = live.empty() ? 0xFFFFFFFFu : live.front().ts;

  // --- сколько строк нужно добрать из файлов ---
  int need;
  if (lastN > 0) {
    need = lastN - (int)live.size();
  } else if (since && oldestLive != 0xFFFFFFFFu) {
    uint32_t saveSec = saveIntervalMs / 1000UL;
    if (saveSec < 60)
      saveSec = 60;
    uint32_t sec = (oldestLive > since) ? (oldestLive - since) : 0;
    need         = (int)(sec / saveSec) + 2;
    if (need > MAX_FILE_ROWS)
      need = MAX_FILE_ROWS;
  } else {
    need = MAX_FILE_ROWS;
  }
  if (need < 0)
    need = 0;

  // --- точки из файлов (только хвосты, файл целиком не читаем) ---
  std::vector<HPoint> rows;
  if (need > 0) {
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);

    std::vector<std::vector<HPoint>> chunks;
    int                             totalRows = 0;
    for (int fi = (int)names.size() - 1; fi >= 0 && totalRows < need; fi--) {
      int perFile = need - totalRows;
      if (perFile > 600)
        perFile = 600;   // кольцо на файл ограничиваем, чтобы не раздувать heap
      std::vector<HPoint> tail;
      readFileTail(names[fi], perFile, tail);
      if (tail.empty())
        continue;
      // файл целиком старше периода — остальные ещё старее, дальше нет смысла
      if (since && tail.back().ts < since)
        break;
      totalRows += (int)tail.size();
      chunks.push_back(std::move(tail));
    }
    for (int i = (int)chunks.size() - 1; i >= 0; i--)
      for (const HPoint &p : chunks[i]) {
        if (p.ts == 0 || p.ts >= oldestLive)
          continue;
        if (since && p.ts < since)
          continue;
        rows.push_back(p);
      }
  }

  // Бюджет точек: редкую историю из файлов бережём, прореживаем только
  // плотную свежую часть из ОЗУ (иначе 12 точек старой части «съедаются»
  // так же, как 360 свежих).
  int liveKeep = (int)live.size();
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
  if (!live.empty()) {
    int step = ((int)live.size() + liveKeep - 1) / liveKeep;
    if (step < 1)
      step = 1;
    int off = ((int)live.size() - 1) % step;
    for (int i = off; i < (int)live.size(); i += step)
      outv.push_back(live[i]);
  }

  // ответ отдаём потоком: JSON целиком в памяти не собираем
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

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Climat Monitor</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--txt:#e2e8f0;--mut:#94a3b8;--acc:#38bdf8;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444}
*{box-sizing:border-box;margin:0;padding:0}
html,body{overflow-x:hidden}
body{font-family:system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh}
header{padding:10px 12px;background:var(--card);display:flex;flex-direction:column;gap:8px;width:100%}
header h1{font-size:24px;font-weight:600;line-height:1.2}
.badges{display:flex;gap:8px;flex-wrap:wrap;min-width:0}
.badge{font-size:12px;padding:3px 10px;border-radius:999px;background:#334155;color:var(--mut);white-space:nowrap}
nav{display:flex;gap:4px;padding:6px 10px;background:var(--card);border-bottom:1px solid #334155;overflow-x:auto;scrollbar-width:none;width:100%}
nav::-webkit-scrollbar{display:none}
nav button{background:none;border:none;color:var(--mut);padding:8px 12px;cursor:pointer;font-size:13px;border-radius:8px;white-space:nowrap;flex:0 0 auto}
nav button.active{background:var(--acc);color:#04263a;font-weight:600}
main{padding:12px;max-width:1100px;margin:0 auto;width:100%}
.tab{display:none}.tab.active{display:block}
.metric{padding:12px}
.mhead{display:flex;align-items:baseline;gap:8px;cursor:pointer;-webkit-user-select:none;user-select:none}

.mlabel{font-size:21px;color:#fff}
.mhead .status{font-size:12px;margin-left:6px}
.mval{margin-left:auto;font-size:29px;font-weight:700;line-height:1.1;text-align:right}
#v-t{color:#f59e0b}
#v-h{color:#38bdf8}
#v-co2{color:#22c55e}
#v-co2.heat{color:var(--bad);font-size:30px}   /* ♨️ вместо цифр во время прогрева */
.marrow{color:var(--mut);font-size:12px;width:14px;text-align:center;flex:0 0 auto}
.mchart{margin-top:10px}
.prow{display:flex;align-items:center;gap:10px;flex-wrap:nowrap}
.prow label{color:var(--mut);font-size:14px;white-space:nowrap}
.prow select,.prow input[type=number]{flex:1 1 auto;min-width:0;width:100%}
.prow select{text-align:right;text-align-last:right;padding-right:34px}
.prow > button.btn{margin-left:auto;margin-right:auto}
.pbtns{justify-content:flex-end;margin-top:10px;flex-wrap:wrap}
input[type=number]{background:#0f172a;border:1px solid #334155;color:var(--txt);padding:9px 12px;border-radius:8px;font-size:16px;text-align:right}
.ok{color:var(--ok)}.bad{color:var(--bad)}.heat{color:var(--bad)}
.panel{background:var(--card);border-radius:14px;padding:12px;margin-bottom:12px;min-width:0}
.panel h2{font-size:14px;margin-bottom:10px;color:var(--acc)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
input[type=text],input[type=password]{background:#0f172a;border:1px solid #334155;color:var(--txt);padding:9px 12px;border-radius:8px;font-size:16px;max-width:100%}
button.btn{background:var(--acc);border:none;color:#04263a;padding:9px 14px;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer}
button.btn.sec{background:#334155;color:var(--txt)}
button.btn.warn{background:var(--warn);color:#241503}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{padding:6px 8px;text-align:left;border-bottom:1px solid #334155;color:var(--mut)}
th{color:var(--txt)}
/* Блок последних записей: прокрутка внутри блока, шапка таблицы закреплена */
.tblwrap{max-height:46vh;overflow:auto;-webkit-overflow-scrolling:touch;border-radius:8px}
.tblwrap thead th{position:sticky;top:0;z-index:1;background:var(--card);box-shadow:inset 0 -1px 0 #334155}
canvas{display:block;width:100%}
select{background:#0f172a;border:1px solid #334155;color:var(--txt);padding:8px;border-radius:8px;max-width:100%}
#msg{font-size:13px;color:var(--ok);min-height:18px;margin-top:8px}
@media(min-width:760px){
 header{padding:14px 24px;flex-direction:row;align-items:center;flex-wrap:wrap;gap:12px}
 .badges{margin-left:auto}
 nav{padding:8px 24px}
 nav button{font-size:14px;padding:8px 16px}
 main{padding:24px}
 .metric{padding:16px}
 .mlabel{font-size:19px}
 .mval{font-size:36px}
 .mhead .status{font-size:13px}
 .panel{border-radius:16px;padding:18px;margin-bottom:16px}
 .panel h2{font-size:15px}
 table{font-size:13px}
 th,td{padding:7px 10px}
}

</style>
</head>
<body>
<header>
  <h1>🌿 Климат-монитор</h1>
  <div class="badges">
    <span class="badge" id="netBadge">—</span>
    <span class="badge" id="logBadge">—</span>
    <span class="badge" id="timeBadge">—</span>
    <span class="badge" id="upBadge">—</span>
  </div>
</header>
<nav>
  <button class="active" data-tab="live" onclick="showTab('live')">Показания</button>
  <button data-tab="wifi" onclick="showTab('wifi')">Wi-Fi</button>
  <button data-tab="calib" onclick="showTab('calib')">Калибровка MH-Z19</button>
  <button data-tab="data" onclick="showTab('data')">Данные</button>
</nav>
<main>

<div class="tab active" id="tab-live">
  <div class="panel metric">
    <div class="mhead" onclick="toggleChart('t')">
      <span class="mlabel">Температура<span class="status" id="s-dht"></span></span>
      <span class="mval" id="v-t">—</span>
      <span class="marrow" id="ar-t">▸</span>
    </div>
    <div class="mchart" id="c-t" style="display:none"><canvas id="ch-t"></canvas></div>
  </div>

  <div class="panel metric">
    <div class="mhead" onclick="toggleChart('h')">
      <span class="mlabel">Влажность</span>
      <span class="mval" id="v-h">—</span>
      <span class="marrow" id="ar-h">▸</span>
    </div>
    <div class="mchart" id="c-h" style="display:none"><canvas id="ch-h"></canvas></div>
  </div>

  <div class="panel metric">
    <div class="mhead" onclick="toggleChart('co2')">
      <span class="mlabel">CO₂<span class="status" id="s-mhz"></span></span>
      <span class="mval" id="v-co2">—</span>
      <span class="marrow" id="ar-co2">▸</span>
    </div>
    <div class="mchart" id="c-co2" style="display:none"><canvas id="ch-co2"></canvas></div>
  </div>

  <div class="panel">
    <div class="prow">
      <label for="rangeSel">Период:</label>
        <select id="rangeSel" onchange="setRange()">
          <option value="6">6 часов</option>
          <option value="12" selected>12 часов</option>
          <option value="24">1 сутки</option>
          <option value="48">2 суток</option>
          <option value="120">5 суток</option>
          <option value="0">Вся история</option>
        </select>
    </div>
  </div>
</div>

<div class="tab" id="tab-wifi">
  <div class="panel">
    <h2>Подключение к домашней сети (STA)</h2>
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px">После подключения устройство перезагрузится и выйдет в интернет через роутер. Точка доступа <b>CO2Monitor</b> остаётся резервным режимом, если сеть недоступна.<br>Если сохранённой сети нет (сразу после прошивки), устройство само попробует подключиться к сети по умолчанию.</p>
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px">Устройство доступно по имени <b>http://climat.local</b> — IP искать не нужно. В режиме точки доступа работает любой адрес (например http://climat.it): встроенный DNS отвечает на все имена.</p>
    <div class="row">
      <input type="text" id="wssid" placeholder="SSID сети">
      <input type="password" id="wpass" placeholder="Пароль">
      <button class="btn" onclick="saveWifi()">Сохранить и подключить</button>
      <button class="btn warn" onclick="clearWifi()">Забыть сеть (режим AP)</button>
    </div>
    <div id="msg"></div>
  </div>
</div>

<div class="tab" id="tab-calib">
  <div class="panel">
    <h2>Ручная калибровка MH-Z19</h2>
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px"><b>Zero calibration (400 ppm)</b> — вынесите датчик на свежий воздух на 20 минут, затем нажмите. Требуется прогрев датчика ≥ 48 часов.<br><b>Span calibration (2000 ppm)</b> — с эталонным газом 2000 ppm.</p>
    <div class="row">
      <button class="btn" onclick="calibrate('zero')">Cal Zero (400 ppm)</button>
      <button class="btn" onclick="calibrate('span')">Cal Span (2000 ppm)</button>
      <button class="btn sec" onclick="calibrate('abc_on')">Автокалибровка ВКЛ</button>
      <button class="btn sec" onclick="calibrate('abc_off')">Автокалибровка ВЫКЛ</button>
    </div>
    <div id="calmsg" style="color:var(--ok);font-size:13px;margin-top:10px;min-height:18px"></div>
  </div>
  <div class="panel">
    <h2>Статус датчика</h2>
    <div class="row"><button class="btn sec" onclick="loadStatus()">Обновить</button></div>
    <div id="calstat" style="font-size:13px;color:var(--mut);margin-top:10px"></div>
  </div>
</div>

<div class="tab" id="tab-data">
  <div class="panel">
    <h2>Настройки записи</h2>
    <div class="prow">
      <label>Точка в память (сек):</label>
      <input type="number" id="setPointSec" min="5" max="600" step="5">
    </div>
    <div class="prow" style="margin-top:10px">
      <label>Сохранение в файл (мин):</label>
      <input type="number" id="setSaveMin" min="1" max="1440" step="1">
    </div>
    <div class="prow pbtns">
      <button class="btn" onclick="saveSettings()">Сохранить</button>
    </div>
    <div id="setmsg" style="font-size:13px;color:var(--ok);min-height:18px;margin-top:8px"></div>
    <p style="color:var(--mut);font-size:12px">Точки в память попадают в графики (плавная линия), но не пишутся во flash. В файл сохраняются реже — для длительной истории.</p>
  </div>
  <div class="panel">
    <h2>Файлы логов (LittleFS)</h2>
    <div id="files" style="font-size:13px;color:var(--mut)">—</div>
    <div class="row" style="margin-top:12px">
      <button class="btn sec" onclick="exportCsv()">Скачать CSV</button>
      <button class="btn warn" onclick="clearLogs()">Удалить все логи</button>
    </div>
  </div>
  <div class="panel"><h2>Последние записи</h2><div class="tblwrap"><table id="tbl"><thead><tr><th>Дата/время</th><th>T °C</th><th>RH %</th><th>CO₂ ppm</th></tr></thead><tbody></tbody></table></div></div>
</div>

</main>
<script>
let hist=[];
let histRange=null;   
let lastSeq=-1;  
let timeOkFlag=true;   // true = на первой загрузке перезагрузка графиков не нужна
let upBase=0, upAt=0;  // время работы: секунды с устройства + момент их получения
let statsFiles=0, statsRam=0;   // записей в файлах и в ОЗУ (из /api/current)
let statsFirst=0, statsLast=0;  // самая ранняя сохранённая и самая новая точка
let currentTab='live';
const $=id=>document.getElementById(id);

const LS_KEY='co2mon.ui';
function saveUi(){
 try{ localStorage.setItem(LS_KEY, JSON.stringify(
   {open:openCharts, range:$('rangeSel').value, tab:currentTab})); }catch(e){}
}
function loadUi(){
 try{
  const u=JSON.parse(localStorage.getItem(LS_KEY)||'{}');
  if(u.range && $('rangeSel').querySelector('option[value="'+u.range+'"]')) $('rangeSel').value=u.range;
  if(u.open) for(const k in MET){
   openCharts[k]=!!u.open[k];
   $(MET[k].box).style.display=openCharts[k]?'block':'none';
   $(MET[k].ar).textContent=openCharts[k]?'▾':'▸';
  }
  if(u.tab) showTab(u.tab);
 }catch(e){}
}
function setRange(){ saveUi(); loadCharts(true); }

function showTab(t){currentTab=t;
document.querySelectorAll('.tab').forEach(e=>e.classList.remove('active'));
$('tab-'+t).classList.add('active');
document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('active',b.dataset.tab===t));
if(t==='data'){loadData();loadSettings();} if(t==='calib')loadStatus();
if(t==='live')drawAll();
saveUi();}

const MET={
 t:{cv:'ch-t',box:'c-t',ar:'ar-t',color:'#f59e0b',label:'°C',field:'t'},
 h:{cv:'ch-h',box:'c-h',ar:'ar-h',color:'#38bdf8',label:'%',field:'h'},
 co2:{cv:'ch-co2',box:'c-co2',ar:'ar-co2',color:'#22c55e',label:'ppm',field:'co2'}
};
let openCharts={t:false,h:false,co2:false};  

function drawOne(k){const m=MET[k];if(!m)return;drawChart(m.cv,m.color,m.label,hist,m.field);}
function drawAll(){for(const k in MET)if(openCharts[k])drawOne(k);}

function toggleChart(k){
 openCharts[k]=!openCharts[k];
 $(MET[k].box).style.display=openCharts[k]?'block':'none';
 $(MET[k].ar).textContent=openCharts[k]?'▾':'▸';
 if(openCharts[k]){drawOne(k);loadCharts(true);}  
 saveUi();
}

function drawChart(id,color,label,data,yfield){
 const cv=$(id); if(!cv)return;

 const h=window.innerWidth<760?190:280;
 cv.style.height=h+'px';
 const dpr=window.devicePixelRatio||1;
 const w=Math.max(160,cv.clientWidth||600);
 cv.width=Math.round(w*dpr); cv.height=Math.round(h*dpr);
 const g=cv.getContext('2d'); g.setTransform(dpr,0,0,dpr,0,0); g.clearRect(0,0,w,h);
 const pad={l:44,r:10,t:16,b:24};
 const vals=data.map(d=>Number(d[yfield])||0);
 let min=Math.min(...vals), max=Math.max(...vals);
 if(!isFinite(min)||!isFinite(max)){min=0;max=1;}
 if(min===max){min-=1;max+=1;}
 const pw=w-pad.l-pad.r, ph=h-pad.t-pad.b;
 g.lineWidth=1; g.strokeStyle='#334155'; g.fillStyle='#94a3b8'; g.font='11px sans-serif';
 for(let i=0;i<=4;i++){
  const y=pad.t+ph*i/4;
  g.beginPath(); g.moveTo(pad.l,y); g.lineTo(w-pad.r,y); g.stroke();
  const v=max-(max-min)*i/4; g.fillText(v.toFixed(1),4,y+4);
 }
 if(data.length){
  // Ось X — по времени, а не по номеру точки: иначе 5-минутные точки старой
  // части и 5-секундные свежие делят ширину поровну и период «не соблюдается».
  const t0=Number(data[0].ts)||0, t1=Number(data[data.length-1].ts)||0;
  const span=(t1>t0)?(t1-t0):0;
  const X=ts=>span?(pad.l+pw*Math.max(0,Math.min(1,(Number(ts)-t0)/span))):pad.l+pw/2;
  const fs=String(data[0].dt||'').slice(5,16), ls=String(data[data.length-1].dt||'').slice(5,16);
  g.fillText(fs,pad.l,h-8);
  g.fillText(ls,w-pad.r-g.measureText(ls).width,h-8);
  g.beginPath(); g.strokeStyle=color; g.lineWidth=2;
  data.forEach((d,i)=>{
   const x=(data.length>1)?X(d.ts):pad.l+pw/2;
   const y=pad.t+ph*(1-(vals[i]-min)/(max-min));
   i?g.lineTo(x,y):g.moveTo(x,y);
  });
  g.stroke();
  g.lineTo(pad.l+pw,pad.t+ph); g.lineTo(pad.l,pad.t+ph); g.closePath();
  g.fillStyle=color+'22'; g.fill();
 }
 g.fillStyle='#e2e8f0'; g.font='12px sans-serif';
 g.fillText(label,pad.l,11);
}

async function api(u){const r=await fetch(u);return r.json();}

async function pollLive(){
 try{
  const s=await api('/api/current');
  $('v-t').innerHTML=s.temperature.toFixed(1);
  $('v-h').innerHTML=s.humidity.toFixed(1);
  const heat=!!s.heating;
  $('v-co2').classList.toggle('heat',heat);
  if(heat){
   $('v-co2').innerHTML='♨️';
   $('s-mhz').innerHTML='<span class="heat">идёт прогрев, '+(s.heatLeft||0)+' с</span>';
  }else{
   $('v-co2').innerHTML=s.co2;
   $('s-mhz').innerHTML=s.mhzOk?'<span class="ok"></span>':'<span class="bad">MH-Z19 нет связи</span>';
  }
  $('s-dht').innerHTML=s.dhtOk?'<span class="ok"></span>':'<span class="bad">DHT22 нет связи</span>';
  $('netBadge').textContent=(s.mode==='AP'?'Точка доступа':'Сеть: '+s.ip)+(s.mdnsOk?' · '+s.mdns:'');
  $('netBadge').title=s.mdnsOk?('Можно открывать по имени: http://'+s.mdns):'mDNS не запущен — используйте IP';
  $('timeBadge').textContent='Дата: '+s.datetime;
  upBase=s.uptime||0; upAt=Date.now();
  statsFiles=s.logRows||0; statsRam=s.points||0;
  statsFirst=s.firstTs||0; statsLast=s.timestamp||0;
  updateLogBadge();
 
  // Часы могли появиться уже после первого запроса истории (тогда сервер отдал
  // всю историю). Как только время известно — перечитываем строго за период.
  if(s.timeOk&&!timeOkFlag){ timeOkFlag=true; histRange=null; loadCharts(true); }
  else if(!s.timeOk) timeOkFlag=false;

  if(s.pointsSeq!==lastSeq){ lastSeq=s.pointsSeq; loadCharts(false); }
 }catch(e){}
 setTimeout(pollLive,5000);}

let chartsTimer=null;
async function loadCharts(full){
 try{
  const hours=$('rangeSel').value;
  if(full||!hist.length||histRange!==hours){
   // полная загрузка: при открытии, смене периода и раз в 2 минуты
   histRange=hours;
   hist=await api('/api/history?hours='+hours);
  }else{
   // живое обновление: догружаем только новые точки (лёгкий запрос)
   const since=hist.length?hist[hist.length-1].ts:0;
   const add=await api('/api/history?since='+since);
   if(add&&add.length)hist=hist.concat(add);
   // окно периода держим всегда, даже если новых точек не пришло
   const hrs=parseFloat(hours)||0;
   const last=hist.length?Number(hist[hist.length-1].ts)||0:0;
   if(hrs>0&&last>0){
    const cut=last-hrs*3600;
    hist=hist.filter(d=>Number(d.ts)>=cut);
   }
   const cap=hrs>0?600:1500;
   if(hist.length>cap)hist=hist.slice(hist.length-cap);
  }
  drawAll();
  updateLogBadge();
 }catch(e){}}

function startChartsLive(){ if(chartsTimer)clearInterval(chartsTimer); chartsTimer=setInterval(()=>loadCharts(true),120000); }

async function loadData(){
 try{
  const files=await api('/api/files');
  $('files').innerHTML=files.length?('Файлов: '+files.length+'<br>'+files.join('<br>')):'Нет файлов';
  const h=await api('/api/history?points=200');
  const tb=$('tbl').querySelector('tbody');tb.innerHTML='';
  h.slice().reverse().forEach(d=>{tb.insertAdjacentHTML('beforeend',
   `<tr><td>${d.dt}</td><td>${d.t.toFixed(1)}</td><td>${d.h.toFixed(1)}</td><td>${d.co2}</td></tr>`);});
 }catch(e){}}

async function saveWifi(){
 const b=new URLSearchParams({ssid:$('wssid').value,pass:$('wpass').value});
 const r=await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
 $('msg').textContent=await r.text();setTimeout(()=>location.reload(),4000);}

async function clearWifi(){
 await fetch('/api/wifi/clear',{method:'POST'});$('msg').textContent='Сеть забыта, возврат в режим AP...';
 setTimeout(()=>location.href='http://192.168.4.1',5000);}

async function calibrate(cmd){
 $('calmsg').textContent='Отправка команды...';
 const r=await fetch('/api/calibrate?type='+cmd);
 $('calmsg').textContent=await r.text();}

async function loadStatus(){
 try{const s=await api('/api/sensor');
  $('calstat').innerHTML=`Диапазон: ${s.range} ppm · ABC: ${s.abc===1||s.abc===true?'вкл':'выкл'} · CO₂ сейчас: ${s.heating?'♨️':s.co2+' ppm'} · Связь: ${s.heating?'прогрев':(s.mhzOk?'OK':'нет данных')}`;
 }catch(e){$('calstat').textContent='Ошибка запроса';}}

async function clearLogs(){
 if(!confirm('Удалить все файлы логов?'))return;
 const r=await fetch('/api/logs/clear',{method:'POST'});
 alert(await r.text());loadData();}

async function loadSettings(){
 try{const s=await api('/api/settings');
  $('setPointSec').value=s.pointSec; $('setSaveMin').value=s.saveMin;
  $('setPointSec').min=s.minPointSec; $('setPointSec').max=s.maxPointSec;
  $('setSaveMin').min=s.minSaveMin; $('setSaveMin').max=s.maxSaveMin;
 }catch(e){}}

async function saveSettings(){
 const b=new URLSearchParams({pointSec:$('setPointSec').value,saveMin:$('setSaveMin').value});
 const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
 $('setmsg').textContent=await r.text(); loadCharts(true);}

function exportCsv(){location.href='/api/export';}

// Время работы с момента включения: показываем посекундно, раз в 5 с
// синхронизируем с устройством (иначе секунды прыгали бы по 5).
function fmtUptime(t){
 t=Math.max(0,Math.floor(t));
 const d=Math.floor(t/86400), h=Math.floor(t%86400/3600), m=Math.floor(t%3600/60), s=t%60;
 const p=n=>String(n).padStart(2,'0');
 return (d>0?d+' д ':'')+p(h)+':'+p(m)+':'+p(s);
}
function tickUptime(){
 if(!upAt)return;
 $('upBadge').textContent='Время работы: '+fmtUptime(upBase+Math.floor((Date.now()-upAt)/1000));
}

// Бейдж показывает, сколько измерений всего сохранено (файлы + ОЗУ), и период
// от самой ранней сохранённой точки до самой новой — а не окно графика.
function logSpan(){
 const t0=Number(statsFirst)||0, t1=Number(statsLast)||0;
 if(!t0||!t1||t1<=t0)return '';
 const sec=t1-t0;
 if(sec<3600)return ' · '+Math.max(1,Math.round(sec/60))+' мин';
 const h=sec/3600;
 return h>=24?(' · '+Math.round(h/24)+' сут'):(' · '+Math.round(h)+' ч');
}
function updateLogBadge(){
 $('logBadge').textContent='Записей: '+(statsFiles+statsRam)+logSpan();
 $('logBadge').title='В файлах: '+statsFiles+' · в ОЗУ: '+statsRam+' · на графике: '+hist.length;
}
window.addEventListener('resize',drawAll);
loadUi();
pollLive();
loadCharts(true);
startChartsLive();
setInterval(tickUptime,1000);
</script>
</body>
</html>)rawliteral";

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/current", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", jsonCurrent());
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    // ?since=<ts> — только новые точки (живое обновление графиков, ответ крошечный)
    if (req->hasParam("since")) {
      uint32_t since = (uint32_t)strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
      req->send(200, "application/json", jsonHistorySince(since));
      return;
    }

    // ?points=N — N последних точек (таблица «Последние записи»)
    if (req->hasParam("points")) {
      int pts = (int)req->getParam("points")->value().toInt();
      pts     = constrain(pts, 10, 20000);
      sendHistory(req, 0, pts);
      return;
    }

    // ?hours=N — строго за последние N часов (0 = вся история)
    int hours = 12;
    if (req->hasParam("hours"))
      hours = req->getParam("hours")->value().toInt();
    uint32_t since = 0;
    // Ориентируемся на само время, а не на флаг timeSynced: после перезагрузки
    // оно может быть уже верным (RTC), а флаг поднимется только на NTP-проверке.
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
    req->send(200, "application/json", listLogFiles());
  });

  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *req) {
    String              all = "timestamp,datetime,temperature,humidity,co2\n";
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);
    for (auto &n : names) {
      File f = LittleFS.open(n, "r");
      if (!f)
        continue;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (!line.startsWith("timestamp"))
          all += line + "\n";
      }
      f.close();
    }
    req->send(200, "text/csv", all);
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
    Serial.println("[CAL] " + resp);
    req->send(200, "text/plain", resp);
  });

  server.on("/api/sensor", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["range"] = mhz.getRange();
    doc["abc"]   = mhz.getABC();
    doc["co2"]   = latest.co2;
    doc["mhzOk"] = mhzOk;
    doc["heating"] = mhzHeating;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (auto &n : names)
      LittleFS.remove(n);
    logRowsTotal = 0;
    oldestSavedTs = 0;
    req->send(200, "text/plain", "Логи удалены");
  });

  server.begin();
}

// mDNS: чтобы открывать веб-интерфейс по имени, а не по IP.
// Работает и в STA, и в AP режиме.
// ВАЖНО: в этом ядре ESPmDNS не подписывается на события сети (подписка
// закомментирована в ESPmDNS.cpp), поэтому если Wi-Fi поднялся ПОСЛЕ старта,
// или произошёл реконнект — mDNS нужно перезапустить вручную (mdnsRestart).
static void startMdns() {
  if (mdnsStarted && !mdnsRestart)
    return;

  static uint32_t lastTry = 0;
  if (!mdnsRestart && lastTry && millis() - lastTry < 10000)
    return;
  lastTry = millis();

  if (mdnsRestart) {
    mdnsRestart = false;
    if (mdnsStarted) {
      MDNS.end();            // перерегистрируемся на новом интерфейсе
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
  mdnsStarted = true;
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
      mdnsRestart = true;   // интерфейс поднялся — перерегистрируем mDNS
    }
  });

  if (cfgSsid.length()) {
    Serial.println("[NET] Пробую подключиться к: " + cfgSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setHostname(MDNS_HOST);   // имя в DHCP: роутеры с локальным DNS дадут climat
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

  // Часы ESP32 (RTC) держат время между перезагрузками, а при прошивке питание
  // не снимается — значит до NTP время уже верное. Без этого первый запрос
  // истории уходил с since=0 и графики строились «за всё время».
  {
    time_t rtcNow = time(nullptr);
    if (rtcNow > 1000000) {
      timeSynced = true;
      Serial.printf("[NTP] время уже есть (RTC): %lu\n", (unsigned long)rtcNow);
    }
  }

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

  recountLogRows();   // сколько записей уже лежит в файлах (один раз на старте)

  dht.begin();

  mhzSerial.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
  heatingStartMs = millis();             // с этого момента считаем преднагрев
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
  latest.timestamp = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;
  lastLogMs        = millis();
  lastPointMs      = millis();
  Serial.printf("[INIT] точка каждые %lu с, сохранение в файл каждые %lu мин\n",
                pointIntervalMs / 1000, saveIntervalMs / 60000);
  Serial.println("[INIT] готово");
}

void loop() {
  if (dnsStarted)
    dnsServer.processNextRequest();

  startMdns();   // если Wi-Fi подключился позже — поднимем mDNS
  static uint32_t lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();
    readSensors();
    tryNtp();
  }

  if (timeSynced && millis() - lastPointMs >= pointIntervalMs) {
    lastPointMs      = millis();
    latest.timestamp = (uint32_t)time(nullptr);
    if ((dhtOk || mhzOk) && !mhzHeating)
      pushLivePoint(latest);
  }

  if (millis() - lastLogMs >= saveIntervalMs) {
    lastLogMs        = millis();
    latest.timestamp = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;
    if ((dhtOk || mhzOk) && !mhzHeating)
      logReading(latest);
  }

  delay(50);
}

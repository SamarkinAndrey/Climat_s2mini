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
#include <esp_now.h>
#include <algorithm>
#include <freertos/semphr.h>
#include <string.h>

#define DHT_PIN            5
#define DHT_TYPE           22
#define MHZ_RX             17
#define MHZ_TX             16
#define LOGS_DIR           "/logs"
#define MAX_LOG_FILES      20
#define MAX_LIST_FILES     32
#define NTP_SERVER         "ru.pool.ntp.org"
#define MDNS_HOST          "climat"
// Опрос датчиков — раз в 5 с. Среднее считается за 5 минут (60 замеров по 5 с).
// Кольцо живых точек хранит последние 24 ч — ровно их и отдаём интерфейсу.
// Запись на флеш — пачкой (flushHourToFlash) раз в saveMin минут.
#define SENSOR_PERIOD_MS   5000UL
#define SAMPLE_PERIOD_SEC  300UL  // период одного среднего: 5 минут
#define SAMPLES_PER_AVG    60     // 60 замеров по 5 с = 5 минут
#define DEFAULT_POINT_SEC  300
#define DEFAULT_SAVE_MIN   60
#define MIN_SAVE_MIN       10
#define MAX_SAVE_MIN       60
#define FLASH_POINTS_MAX   12     // максимум точек в буфере флеша (60 мин / 5 мин)
#define MAX_LIVE_POINTS    288    // 24 ч пятиминутных средних в ОЗУ (24*60/5)
#define MAX_HISTORY_OUT    300
#define FRONT_CAP_PERIOD   400
#define FRONT_CAP_ALL      800
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
bool     mhzFirstReadDone = false;
bool     mhzHeating      = true;
bool     mdnsStarted     = false;
bool     mdnsRestart     = false;
uint32_t staDownSinceMs  = 0;   // с какого момента нет STA-связи (0 — связь есть)
uint32_t lastStaRetryMs  = 0;   // время последней попытки переподключения
uint32_t lastHeartbeatMs = 0;   // время последнего сердцебиения в лог
uint8_t  lastStaDiscReason = 0; // код последнего отключения STA (Wi-Fi reason)
uint32_t heatingStartMs  = 0;
uint32_t lastLogMs       = 0;
uint32_t lastNtpTry      = 0;
uint32_t lastPointMs     = 0;
uint16_t accN            = 0; // тиков накопления в текущую минуту (из 12)
bool     apMode          = true;
bool     timeSynced      = false;
uint32_t saveIntervalMs  = DEFAULT_SAVE_MIN * 60UL * 1000UL;
uint32_t pointsSeq       = 0;
uint32_t logRowsTotal    = 0;
uint32_t oldestSavedTs   = 0;

struct LogFileEntry {
  String name;
  size_t size;
  bool   ext;
};
static LogFileEntry logFilesCache[MAX_LIST_FILES];
static int          logFilesCacheCount = 0;
static bool         logFilesCacheDirty = true;
static String       logFilesCacheJson = "[]";

// Кэш списка файлов живёт в ОЗУ: при обновлении интерфейса LittleFS не читается.
// Пересобирается только на старте и при изменениях. Размеры файлов обновляются
// инкрементально в момент записи (flushHourToFlash), без открытия файлов.
static int cacheFind(const String &name) {
  for (int i = 0; i < logFilesCacheCount; i++)
    if (logFilesCache[i].name == name)
      return i;
  return -1;
}

static void cacheToJson() {
  String out = "[";
  for (int i = logFilesCacheCount - 1; i >= 0; i--) { // новые сверху
    if (i < logFilesCacheCount - 1)
      out += ",";
    out += "{\"name\":\"" + logFilesCache[i].name + "\",\"size\":" +
           String((unsigned)logFilesCache[i].size) +
           ",\"ext\":" + (logFilesCache[i].ext ? "true" : "false") + "}";
  }
  out += "]";
  logFilesCacheJson = out;
}

static void cacheAddBytes(const String &name, size_t add, bool ext) {
  if (logFilesCacheDirty)
    return; // кэш ещё не построен — пересоберём целиком при первом запросе
  int idx = cacheFind(name);
  if (idx >= 0) {
    logFilesCache[idx].size += add;
    logFilesCache[idx].ext = ext;
    cacheToJson();
    return;
  }
  if (logFilesCacheCount >= MAX_LIST_FILES)
    return;
  int pos = logFilesCacheCount;
  while (pos > 0 && logFilesCache[pos - 1].name > name) {
    logFilesCache[pos] = logFilesCache[pos - 1];
    pos--;
  }
  logFilesCache[pos] = {name, add, ext};
  logFilesCacheCount++;
  cacheToJson();
}

static void cacheAddPath(const String &path, size_t add, bool ext) {
  int    slash = path.lastIndexOf('/');
  String name  = slash >= 0 ? path.substring(slash + 1) : path;
  cacheAddBytes(name, add, ext);
}

static void cacheRemovePath(const String &path) {
  if (logFilesCacheDirty)
    return;
  int    slash = path.lastIndexOf('/');
  String name  = slash >= 0 ? path.substring(slash + 1) : path;
  int    idx   = cacheFind(name);
  if (idx < 0)
    return;
  for (int i = idx; i < logFilesCacheCount - 1; i++)
    logFilesCache[i] = logFilesCache[i + 1];
  logFilesCacheCount--;
  cacheToJson();
}

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

// ---------- Внешний блок (ESP32-C3 + BME280), приём по ESP-NOW ----------
// Пакет одинаковой структуры на обеих платах (см. src/external_block.ino).
struct SensorPacket {
  uint32_t magic;     // 0x434C4931 ("CLI1")
  float    temp;      // температура внешнего блока, °C
  float    hum;       // влажность внешнего блока, %
  float    press;     // давление, гПа
  float    batt;      // напряжение аккумулятора, В
  uint32_t uptime;    // секунды работы внешнего блока с последней перезагрузки
  uint32_t timestamp; // 0 — внутренняя плата подставит своё время
};

#define EXT_MAGIC   0x434C4931UL
#define MAX_EXT_PTS 288 // 24 часа пятиминутных средних внешнего блока

struct ExtAvg {
  float    t, h, p, b;
  uint32_t ts;
};

static ExtAvg extBuf[MAX_EXT_PTS];
static int    extHead  = 0;
static int    extCount = 0;
static uint32_t extSeq = 0;
static bool   extSeen = false;      // хоть один пакет принят
static uint32_t extLastRxMs = 0;    // millis() последнего пакета
static portMUX_TYPE extMux = portMUX_INITIALIZER_UNLOCKED;

// Аккумуляторы средних за период (внутренний и внешний блоки)
struct MinAcc {
  float    t = 0, h = 0, p = 0, b = 0;
  uint32_t co2   = 0;
  uint16_t nTh   = 0; // успешных измерений DHT за минуту
  uint16_t nCo2  = 0; // успешных измерений MH-Z19 за минуту
  uint16_t nExt  = 0; // пакетов внешнего блока за минуту
};
static MinAcc accIn, accExt;

struct MinuteAvg {
  float    t, h;
  uint16_t co2;
  uint32_t ts;
};
static MinuteAvg    hourBuf[FLASH_POINTS_MAX];
static uint8_t      hourCount = 0;
static ExtAvg extHourBuf[FLASH_POINTS_MAX];
static uint8_t      extHourCount = 0;

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

// Статический список файлов логов (без std::vector — не фрагментируем кучу).
static int listFiles(const char *dirPath, String *names, int maxNames) {
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory())
    return 0;
  int cnt = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    String name  = String(entry.name());
    int    slash = name.lastIndexOf('/');
    if (slash >= 0)
      name = name.substring(slash + 1);
    if (name.length() && cnt < maxNames)
      names[cnt++] = String(dirPath) + "/" + name;
    entry.close();
  }
  dir.close();
  return cnt;
}

static void sortNames(String *names, int n) {
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (names[j] < names[i])
        std::swap(names[i], names[j]);
}

// Файлы внешнего блока (/logs/ext_*.csv) имеют другую схему:
// timestamp,datetime,temperature,humidity,pressure,batt
// Их нельзя смешивать с основными (5 колонок) — иначе давление попадёт в CO2.
static bool isExtLogFile(const String &path) {
  int slash = path.lastIndexOf('/');
  String base = (slash >= 0) ? path.substring(slash + 1) : path;
  return base.startsWith("ext_");
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

// Имя файла приходит из запроса — не даём выйти за пределы /logs.
static bool isSafeLogName(const String &name) {
  if (name.length() == 0 || name.length() > 64)
    return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0)
    return false;
  return name.endsWith(".csv");
}

// Самая ранняя метка времени среди основных логов (ext_* не учитываем).
static uint32_t oldestMainTs() {
  static String names[MAX_LIST_FILES];
  int n = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  int oi = -1;
  for (int i = 0; i < n; i++) {
    if (isExtLogFile(names[i]))
      continue;
    if (oi < 0 || names[i] < names[oi])
      oi = i;
  }
  return oi >= 0 ? firstFileTs(names[oi]) : 0;
}

static void recountLogRows() {
  static String names[MAX_LIST_FILES];
  int n = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  uint32_t total = 0;
  int      main  = 0;
  for (int i = 0; i < n; i++) {
    if (isExtLogFile(names[i]))
      continue; // строки внешнего блока считаются отдельно
    total += countFileRows(names[i]);
    main++;
  }
  logRowsTotal = total;

  oldestSavedTs = 0;
  if (main > 0) {
    int oi = -1;
    for (int i = 0; i < n; i++) {
      if (isExtLogFile(names[i]))
        continue;
      if (oi < 0 || names[i] < names[oi])
        oi = i;
    }
    if (oi >= 0)
      oldestSavedTs = firstFileTs(names[oi]);
  }
  logFilesCacheDirty = true;
  Serial.printf("[LOG] файлов %u (основных %d), записей всего %lu, самая ранняя %lu\n",
                (unsigned)n, main, (unsigned long)total, (unsigned long)oldestSavedTs);
}

static void rotateLogsIfNeeded() {
  static String names[MAX_LIST_FILES];
  int n = listFiles(LOGS_DIR, names, MAX_LIST_FILES);

  int removed = 0;
  while (n > MAX_LOG_FILES && removed < 2) {
    int oldestIdx = 0;
    for (int i = 1; i < n; i++)
      if (names[i] < names[oldestIdx])
        oldestIdx = i;
    Serial.printf("[LOG] ротация: удаляю %s\n", names[oldestIdx].c_str());
    uint32_t gone = countFileRows(names[oldestIdx]);
    logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
    if (LittleFS.remove(names[oldestIdx]))
      cacheRemovePath(names[oldestIdx]);
    for (int i = oldestIdx; i < n - 1; i++)
      names[i] = names[i + 1];
    n--;
    removed++;
    oldestSavedTs = 0;
    if (n > 0) {
      int oi = 0;
      for (int i = 1; i < n; i++)
        if (names[i] < names[oi])
          oi = i;
      oldestSavedTs = firstFileTs(names[oi]);
    }
  }
}

static String extTodayFile() {
  time_t    now = time(nullptr);
  struct tm t;
  if (timeSynced && now > 1000000) {
    localtime_r(&now, &t);
  } else {
    memset(&t, 0, sizeof(t));
    t.tm_year = 100;
    t.tm_mon  = 0;
    t.tm_mday = 1;
  }
  char name[40];
  sprintf(name, "%s/ext_%04d-%02d-%02d.csv", LOGS_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(name);
}

// Запись отдельного «сырого» замера в файл больше не используется:
// в файлы попадают только средние за период (flushHourToFlash).
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

static void pushExtAvg(const ExtAvg &a) {
  portENTER_CRITICAL(&extMux);
  ExtAvg &p = extBuf[extHead];
  p         = a;
  extHead   = (extHead + 1) % MAX_EXT_PTS;
  if (extCount < MAX_EXT_PTS)
    extCount++;
  extSeq++;
  portEXIT_CRITICAL(&extMux);
}

static void extSnapshot(int &head, int &count) {
  portENTER_CRITICAL(&extMux);
  head  = extHead;
  count = extCount;
  portEXIT_CRITICAL(&extMux);
}

static ExtAvg extGet(int idx) {
  ExtAvg a;
  portENTER_CRITICAL(&extMux);
  a = extBuf[idx];
  portEXIT_CRITICAL(&extMux);
  return a;
}

// ---------- ESP-NOW: приём пакетов внешнего блока ----------
// Сигнатура под Arduino-ESP32 core 2.x (esp_now_recv_cb_t): mac, data, len.
// В core 3.x первым аргументом приходит esp_now_recv_info_t* — см. аудит.
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  if (len != sizeof(SensorPacket))
    return;
  SensorPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != EXT_MAGIC)
    return;
  // значения должны быть физическими, иначе пакет битый
  if (!(pkt.temp > -60 && pkt.temp < 60))
    return;
  if (!(pkt.hum >= 0 && pkt.hum <= 100))
    return;

  uint32_t ts = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;
  portENTER_CRITICAL(&extMux);
  extSeen     = true;
  extLastRxMs = millis();
  // Накопление внешних измерений защищено той же блокировкой, что и кольцо
  // extBuf: колбэк ESP-NOW живёт в задаче Wi-Fi и раньше писал в accExt
  // без синхронизации с accumulateAvg() — это была гонка по float.
  accExt.t += pkt.temp;
  accExt.h += pkt.hum;
  accExt.p += pkt.press;
  accExt.b += pkt.batt;
  accExt.nExt++;
  portEXIT_CRITICAL(&extMux);
  (void)ts;
}

static void setupEspNow() {
  // ESP-NOW работает поверх уже поднятого интерфейса. Держим AP+STA, чтобы
  // приём работал независимо от того, подключился ли STA к домашней сети.
  WiFi.mode(WIFI_AP_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ошибка инициализации — внешний блок не принимается");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("[ESPNOW] приём включён, MAC этой платы: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    mhzFirstReadDone = true;    
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

// Пятиминутные средние: вызывается из loop() каждые SENSOR_PERIOD_MS.
// Раз в SAMPLES_PER_AVG тиков закрывает интервал: среднее идёт в liveBuf/extBuf
// (для графиков и /api/history?since=) и в буферы для записи на флеш.
static void accumulateAvg() {
  Reading r = getLatest();
  bool    thOk  = dhtOk && !isnan(r.temperature) && !isnan(r.humidity);
  bool    co2Ok = mhzOk && !mhzHeating && r.co2 > 0;

  if (thOk) {
    accIn.t += r.temperature;
    accIn.h += r.humidity;
    accIn.nTh++;
  }
  if (co2Ok) {
    accIn.co2 += r.co2;
    accIn.nCo2++;
  }

  // внешний блок считаем живым, если пакет был за последние 15 минут.
  // Снимок накопителя берём под блокировкой — колбэк ESP-NOW пишет в accExt
  // из задачи Wi-Fi, здесь же он читается и обнуляется.
  bool   extAlive;
  MinAcc extAcc;
  portENTER_CRITICAL(&extMux);
  extAlive = extSeen && (millis() - extLastRxMs < 15UL * 60UL * 1000UL);
  extAcc   = accExt;
  if (!extAlive) {
    accExt.nExt = 0;
    accExt.t = accExt.h = accExt.p = accExt.b = 0;
  }
  portEXIT_CRITICAL(&extMux);

  if (++accN < SAMPLES_PER_AVG)
    return;

  // ---- интервал среднего закрылся ----
  uint32_t ts = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;

  if (accIn.nTh || accIn.nCo2) {
    float avgT   = accIn.nTh ? accIn.t / accIn.nTh : r.temperature;
    float avgH   = accIn.nTh ? accIn.h / accIn.nTh : r.humidity;
    // Нет ни одного валидного замера CO2 за интервал (датчик отключён/прогрев/брак) —
    // оставляем 0 как признак «нет данных». Наружу он уходит как null, в CSV — пустым.
    uint16_t avgCo2 = accIn.nCo2 ? (uint16_t)(accIn.co2 / accIn.nCo2) : 0;

    Reading avg;
    avg.timestamp   = ts;
    avg.temperature = avgT;
    avg.humidity    = avgH;
    avg.co2         = avgCo2;
    pushLivePoint(avg);

    if (hourCount < FLASH_POINTS_MAX) {
      hourBuf[hourCount].t   = avgT;
      hourBuf[hourCount].h   = avgH;
      hourBuf[hourCount].co2 = avgCo2;
      hourBuf[hourCount].ts  = ts;
      hourCount++;
    } else {
      // буфер переполнен (предыдущий сброс не удался) — данные не пишем,
      // но сообщаем, чтобы не терять их молча
      static uint32_t lastOverflowWarn = 0;
      if (millis() - lastOverflowWarn > 60000UL) {
        lastOverflowWarn = millis();
        Serial.println("[LOG] буфер флеша переполнен — точки теряются (ФС занята?)");
      }
    }
  }

  if (extAcc.nExt) {
    ExtAvg ea;
    ea.t  = extAcc.t / extAcc.nExt;
    ea.h  = extAcc.h / extAcc.nExt;
    ea.p  = extAcc.p / extAcc.nExt;
    ea.b  = extAcc.b / extAcc.nExt;
    ea.ts = ts;
    pushExtAvg(ea);
    if (extHourCount < FLASH_POINTS_MAX) {
      extHourBuf[extHourCount] = ea;
      extHourCount++;
    }
  }

  accIn = MinAcc();
  accN  = 0;
  portENTER_CRITICAL(&extMux);
  accExt = MinAcc();
  portEXIT_CRITICAL(&extMux);
}

// Запись пачки пятиминутных средних на флеш (~288 строк/сутки).
static void flushHourToFlash() {
  bool locked = fsLock(500);
  if (!locked) {
    Serial.println("[LOG] ФС занята, сброс часа отложен");
    return;
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  if (hourCount > 0) {
    String path    = todayFile();
    bool   isNew   = !LittleFS.exists(path);
    File   f       = LittleFS.open(path, "a");
    if (f) {
      size_t written = 0;
      if (isNew) {
        int n = f.println("timestamp,datetime,temperature,humidity,co2");
        if (n > 0)
          written += (size_t)n;
      }
      for (uint8_t i = 0; i < hourCount; i++) {
        // 0 в CO2 — «нет валидного замера»: в файл пишем пустое поле, а не ложный 0.
        int n;
        if (hourBuf[i].co2)
          n = f.printf("%lu,%s,%.2f,%.2f,%u\n", hourBuf[i].ts, fmtTime(hourBuf[i].ts).c_str(),
                       hourBuf[i].t, hourBuf[i].h, hourBuf[i].co2);
        else
          n = f.printf("%lu,%s,%.2f,%.2f,\n", hourBuf[i].ts, fmtTime(hourBuf[i].ts).c_str(),
                       hourBuf[i].t, hourBuf[i].h);
        if (n > 0)
          written += (size_t)n;
      }
      f.close();
      logRowsTotal += hourCount;
      cacheAddPath(path, written, false); // размер обновляем без чтения флеша
      Serial.printf("[LOG] сброс буфера: %u строк в %s\n", hourCount, path.c_str());
      hourCount = 0;
    } else {
      Serial.println("[LOG] ошибка открытия файла, сброс отложен");
    }
  }

  if (extHourCount > 0) {
    String path    = extTodayFile();
    bool   isNew   = !LittleFS.exists(path);
    File   f       = LittleFS.open(path, "a");
    if (f) {
      size_t written = 0;
      if (isNew) {
        int n = f.println("timestamp,datetime,temperature,humidity,pressure,batt");
        if (n > 0)
          written += (size_t)n;
      }
      for (uint8_t i = 0; i < extHourCount; i++) {
        int n = f.printf("%lu,%s,%.2f,%.2f,%.1f,%.2f\n", extHourBuf[i].ts,
                         fmtTime(extHourBuf[i].ts).c_str(), extHourBuf[i].t, extHourBuf[i].h,
                         extHourBuf[i].p, extHourBuf[i].b);
        if (n > 0)
          written += (size_t)n;
      }
      f.close();
      cacheAddPath(path, written, true);
      Serial.printf("[LOG] сброс буфера ext: %u строк в %s\n", extHourCount, path.c_str());
      extHourCount = 0;
    }
  }

  rotateLogsIfNeeded();
  fsUnlock();
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
    // До синхронизации средние накапливались с ts=0 — в файлы такие строки
    // попадать не должны. Сбрасываем накопленное и начинаем заново.
    accIn       = MinAcc();
    accN        = 0;
    hourCount   = 0;
    extHourCount = 0;
    portENTER_CRITICAL(&extMux);
    accExt = MinAcc();
    portEXIT_CRITICAL(&extMux);
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

static uint32_t heatingLeftSec() {
  uint32_t elapsed = millis() - heatingStartMs;
  if (elapsed >= MHZ19_HEATING_TIME) return 0;
  return (MHZ19_HEATING_TIME - elapsed) / 1000UL;
}

static size_t jsonCurrent(char *buf, size_t n) {
  JsonDocument doc;

  // heating и heatLeft считаем из одного свежего elapsed: mhzHeating обновляется
  // только раз в 5 с в readSensors(), поэтому на границе прогрева heatLeft уходил в мусор.
  uint32_t heatElapsed = millis() - heatingStartMs;
  bool     heatingNow  = (heatElapsed < MHZ19_HEATING_TIME);

  Reading      r     = getLatest();
  doc["temperature"] = r.temperature;
  doc["humidity"]    = r.humidity;
  // 0 — признак «нет валидного замера», отдаём null (в UI будет «—»).
  if (r.co2)
    doc["co2"] = r.co2;
  else
    doc["co2"] = nullptr;
  doc["dhtOk"]       = dhtOk;
  doc["mhzOk"]       = mhzOk;
  doc["mhzReady"]    = mhzFirstReadDone;

  uint32_t heatLeft = heatingLeftSec();
  doc["heating"]     = (heatLeft > 0);
  doc["heatLeft"]    = heatLeft;

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
  doc["pointSec"]     = DEFAULT_POINT_SEC;
  doc["saveMin"]      = saveIntervalMs / 60000;
  doc["frontCap"]     = FRONT_CAP_PERIOD;
  doc["capPeriod"]    = FRONT_CAP_PERIOD;
  doc["capAll"]       = FRONT_CAP_ALL;
  int lvHead, lvCount;
  liveSnapshot(lvHead, lvCount);
  doc["points"] = lvCount;

  // Внешний блок (ESP-NOW): последние средние + статус связи
  {
    int eh, ec;
    extSnapshot(eh, ec);
    bool alive;
    uint32_t ago = 0;
    portENTER_CRITICAL(&extMux);
    alive = extSeen && (millis() - extLastRxMs < 15UL * 60UL * 1000UL);
    if (extSeen)
      ago = (uint32_t)((millis() - extLastRxMs) / 1000UL);
    portEXIT_CRITICAL(&extMux);
    doc["extOnline"]  = alive;
    doc["extSeen"]    = extSeen;
    doc["extAgoSec"]  = ago;
    doc["extSeq"]     = extSeq;
    doc["extPoints"]  = ec;
    JsonObject ep     = doc["ext"].to<JsonObject>();
    if (ec > 0) {
      ExtAvg a = extGet((eh - 1 + MAX_EXT_PTS) % MAX_EXT_PTS);
      ep["t"]     = a.t;
      ep["h"]     = a.h;
      ep["p"]     = a.p;
      ep["batt"]  = a.b;
      ep["ts"]    = a.ts;
    } else {
      // свежий усреднённый пакет ещё не закрылся минутой — показываем аккумуляторы
      uint16_t n = accExt.nExt;
      if (alive && n > 0) {
        ep["t"]    = accExt.t / n;
        ep["h"]    = accExt.h / n;
        ep["p"]    = accExt.p / n;
        ep["batt"] = accExt.b / n;
      } else {
        ep["t"]   = nullptr;
        ep["h"]   = nullptr;
        ep["p"]   = nullptr;
        ep["batt"] = nullptr;
      }
    }
    ep["online"] = alive;
  }

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

// co2 == 0 — служебный признак «валидного замера не было» (датчик отключён,
// идёт прогрев или все чтения забракованы). В JSON отдаём null, чтобы
// график/таблица не показывали ложный 0 ppm.
static void fmtPoint(char *buf, size_t n, const HPoint &p) {
  char dt[24];
  fmtTimeBuf(p.ts, dt, sizeof(dt));
  if (p.co2)
    snprintf(buf, n,
             "{\"ts\":%lu,\"dt\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"co2\":%u}",
             (unsigned long)p.ts, dt, p.t, p.h, p.co2);
  else
    snprintf(buf, n,
             "{\"ts\":%lu,\"dt\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"co2\":null}",
             (unsigned long)p.ts, dt, p.t, p.h);
}

// Складывает одну точку JSON в строку. Раньше это писалось в AsyncResponseStream,
// но тот отдаёт тело побайтово (StreamString::read -> remove(0,1)), т.е. за O(n^2):
// на ответе в десятки КБ задача async_tcp не успевала сбросить watchdog -> abort.
static void appendPoint(String &out, const HPoint &p, bool &first) {
  if (!first)
    out += ',';
  first = false;
  char buf[160];
  fmtPoint(buf, sizeof(buf), p);
  out += buf;
}

// Средние внешнего блока (для /api/ext_history): кольцо + снимок.
struct ExtPt {
  uint32_t ts;
  float    t, h, p, b;
};

static void extPointsSince(uint32_t since, ExtPt *out, int maxOut, int &nOut) {
  nOut = 0;
  int eh, ec;
  extSnapshot(eh, ec);
  int start = (eh - ec + MAX_EXT_PTS) % MAX_EXT_PTS;
  for (int i = 0; i < ec && nOut < maxOut; i++) {
    ExtAvg a = extGet((start + i) % MAX_EXT_PTS);
    if (a.ts <= since)
      continue;
    out[nOut++] = ExtPt{a.ts, a.t, a.h, a.p, a.b};
  }
}

static void sendExtHistory(AsyncWebServerRequest *req, uint32_t since) {
  static ExtPt g_extOut[MAX_HISTORY_OUT];
  int n = 0;
  extPointsSince(since, g_extOut, MAX_HISTORY_OUT, n);
  // Тело собираем в String и отдаём через AsyncBasicResponse (req->send): он копирует
  // данные чанками, а не по одному байту, и не блокирует async_tcp дольше watchdog.
  String out;
  out.reserve(16 + (size_t)n * 112);
  out += '[';
  char dt[24];
  char buf[176];
  for (int i = 0; i < n; i++) {
    if (i)
      out += ',';
    fmtTimeBuf(g_extOut[i].ts, dt, sizeof(dt));
    snprintf(buf, sizeof(buf),
             "{\"ts\":%lu,\"dt\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"p\":%.1f,\"batt\":%.2f}",
             (unsigned long)g_extOut[i].ts, dt, g_extOut[i].t, g_extOut[i].h,
             g_extOut[i].p, g_extOut[i].b);
    out += buf;
  }
  out += ']';
  req->send(200, "application/json", out.c_str());
}

static void sendHistorySince(AsyncWebServerRequest *req, uint32_t since) {
  static HPoint g_sinceOut[MAX_HISTORY_OUT];
  int lh, lc;
  liveSnapshot(lh, lc);
  int start = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;

  // Инкремент: все новые точки без прореживания (обычно 0–1 точек за 5 мин).
  int outCount = 0;
  for (int i = 0; i < lc && outCount < MAX_HISTORY_OUT; i++) {
    Point sp = liveGet((start + i) % MAX_LIVE_POINTS);
    if (sp.ts <= since)
      continue;
    g_sinceOut[outCount++] = HPoint{sp.ts, sp.t, sp.h, sp.co2};
  }

  String out;
  out.reserve(16 + (size_t)outCount * 96);
  out += '[';
  bool firstItem = true;
  for (int i = 0; i < outCount; i++)
    appendPoint(out, g_sinceOut[i], firstItem);
  out += ']';
  req->send(200, "application/json", out.c_str());
}

// ---------- Предзагрузка истории из файлов ----------
// Обработчик /api/history работает только с ОЗУ: длинное чтение флеша внутри
// задачи async_tcp роняло устройство по сторожевому таймеру (task_wdt -> abort).
// Чтобы график не был пустым сразу после перезагрузки, при старте один раз
// заполняем кольцо liveBuf последними MAX_LIVE_POINTS пятиминутными строками.
// Чтение — крупными блоками (как countFileRows), поэтому старт не тормозит.
static void preloadLiveFromFlash() {
  static String names[MAX_LIST_FILES];
  int nf = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  sortNames(names, nf);

  static HPoint  tmp[MAX_LIVE_POINTS];
  static uint8_t chunk[512];
  static char    line[176];
  int            cnt = 0;

  for (int i = 0; i < nf; i++) {
    if (isExtLogFile(names[i]))
      continue;
    File f = LittleFS.open(names[i], "r");
    if (!f)
      continue;
    size_t ll     = 0;
    bool   header = true;
    while (true) {
      int n = f.read(chunk, sizeof(chunk));
      if (n <= 0)
        break;
      for (int k = 0; k < n; k++) {
        char c = (char)chunk[k];
        if (c == '\r')
          continue;
        if (c != '\n') {
          if (ll < sizeof(line) - 1)
            line[ll++] = c;
          else
            ll = 0; // слишком длинная строка — пропускаем
          continue;
        }
        if (!ll)
          continue;
        line[ll] = 0;
        ll       = 0;
        if (header) { // первая строка — заголовок
          header = false;
          continue;
        }
        uint32_t ts = (uint32_t)strtoul(line, nullptr, 10);
        if (!ts)
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
        char *c4 = strchr(c3 + 1, ','); // всё после — CO2
        if (!c4)
          continue;
        HPoint p{ts, (float)atof(c2 + 1), (float)atof(c3 + 1), (uint16_t)atoi(c4 + 1)};
        if (cnt < MAX_LIVE_POINTS) {
          tmp[cnt++] = p;
        } else { // держим только последние MAX_LIVE_POINTS строк
          memmove(tmp, tmp + 1, (MAX_LIVE_POINTS - 1) * sizeof(HPoint));
          tmp[MAX_LIVE_POINTS - 1] = p;
        }
      }
    }
    f.close();
  }

  if (cnt == 0)
    return;

  portENTER_CRITICAL(&liveMux);
  liveHead  = 0;
  liveCount = 0;
  for (int i = 0; i < cnt; i++) {
    Point &p = liveBuf[liveHead];
    p.ts     = tmp[i].ts;
    p.t      = tmp[i].t;
    p.h      = tmp[i].h;
    p.co2    = tmp[i].co2;
    liveHead = (liveHead + 1) % MAX_LIVE_POINTS;
    liveCount++;
  }
  portEXIT_CRITICAL(&liveMux);
  pointsSeq++;

  Serial.printf("[HIST] из файлов предзагружено %d точек (последние ~%lu ч)\n",
                cnt, (unsigned long)((uint32_t)cnt * SAMPLE_PERIOD_SEC / 3600UL));
}

// ---------- Сборка истории /api/history ----------
// Только кольцо liveBuf: последние 24 ч пятиминутных средних (MAX_LIVE_POINTS
// = 288), плюс предзагруженное из файлов при старте. Флеш здесь не читается:
// обработчик выполняется в задаче async_tcp, и длинное чтение файлов роняло
// устройство по сторожевому таймеру. Объём ответа — не больше MAX_HISTORY_OUT
// точек, т.е. не больше последних 24 ч.
static void sendHistory(AsyncWebServerRequest *req, uint32_t since, int lastN) {
  const int maxOut = (lastN > 0) ? lastN : MAX_HISTORY_OUT;

  // ---- окно живых средних (кольцо хранит последние 24 ч) ----
  int lh, lc;
  liveSnapshot(lh, lc);
  const int ringStart = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;

  int newestFirst = 0;
  int L           = 0;
  for (int i = lc - 1; i >= 0; i--) {
    Point sp = liveGet((ringStart + i) % MAX_LIVE_POINTS);
    if (sp.ts == 0)
      break; // точки без времени (до NTP) не участвуют
    if (since && sp.ts < since)
      break;
    if (!L)
      newestFirst = i; // самая свежая принятая точка
    L++;
  }
  // индекс кольца самой старой принятой живой точки
  const int liveBegin = (ringStart + newestFirst - L + 1 + MAX_LIVE_POINTS * 2) % MAX_LIVE_POINTS;

  // ---- выбор точек: points=N — последние N, иначе всё окно (<= 24 ч) ----
  int lFrom = 0, lCount = L;
  if (lastN > 0 && lCount > maxOut) {
    lFrom  = L - maxOut;
    lCount = maxOut;
  }

  String out;
  out.reserve(16 + (size_t)lCount * 96);
  out += '[';
  bool firstItem = true;
  for (int i = 0; i < lCount; i++) {
    Point sp = liveGet((liveBegin + lFrom + i) % MAX_LIVE_POINTS);
    appendPoint(out, HPoint{sp.ts, sp.t, sp.h, sp.co2}, firstItem);
  }
  out += ']';
  req->send(200, "application/json", out.c_str());
}

// Пересборка кэша. Вызывать только под fsLock: читает каталог и размеры файлов.
static void rebuildLogFilesCache() {
  static String names[MAX_LIST_FILES];
  int nf = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  sortNames(names, nf);
  logFilesCacheCount = 0;
  for (int i = 0; i < nf && logFilesCacheCount < MAX_LIST_FILES; i++) {
    int    slash = names[i].lastIndexOf('/');
    String base  = slash >= 0 ? names[i].substring(slash + 1) : names[i];
    bool   extf  = isExtLogFile(names[i]);
    size_t fsize = 0;
    File   f     = LittleFS.open(names[i], "r");
    if (f) {
      fsize = f.size();
      f.close();
    }
    logFilesCache[logFilesCacheCount++] = {base, fsize, extf};
  }
  cacheToJson();
  logFilesCacheDirty = false;
}

// Вызывать только под fsLock.
static String listLogFiles() {
  if (logFilesCacheDirty)
    rebuildLogFilesCache();
  return logFilesCacheJson;
}

extern const uint8_t        index_html_start[] asm("_binary_src_internal_index_html_start");
extern const uint8_t        index_html_end[] asm("_binary_src_internal_index_html_end");
static const uint8_t *const INDEX_HTML     = index_html_start;
static const size_t         INDEX_HTML_LEN = (size_t)(index_html_end - index_html_start);

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(new AsyncProgmemResponse(200, "text/html", INDEX_HTML, INDEX_HTML_LEN));
  });

  server.on("/api/current", HTTP_GET, [](AsyncWebServerRequest *req) {
    char buf[1024];
    size_t n = jsonCurrent(buf, sizeof(buf));
    if (n == 0 || (size_t)n >= sizeof(buf))
      Serial.println("[API] /api/current: ответ не влез в буфер");
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

    int hours = 24; // интерфейсу отдаём последние сутки — не больше
    if (req->hasParam("hours"))
      hours = constrain(req->getParam("hours")->value().toInt(), 1, 24);
    uint32_t since = 0;
    if (time(nullptr) > 1000000) {
      since = (uint32_t)time(nullptr) - (uint32_t)hours * 3600UL;
    } else {
      since = 0;
    }
    Serial.printf("[HIST] /api/history: период %d ч (since=%lu)\n",
                  hours, (unsigned long)since);
    sendHistory(req, since, 0);
  });

  server.on("/api/ext_history", HTTP_GET, [](AsyncWebServerRequest *req) {
    uint32_t since = 0;
    if (req->hasParam("since"))
      since = (uint32_t)strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
    else {
      int hours = 24;
      if (req->hasParam("hours"))
        hours = req->getParam("hours")->value().toInt();
      if (hours > 0 && time(nullptr) > 1000000)
        since = (uint32_t)time(nullptr) - (uint32_t)hours * 3600UL - 60;
    }
    sendExtHistory(req, since);
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    // Период точки фиксирован: точки — пятиминутные средние (60 замеров по 5 с).
    // Значение отдаётся для совместимости со старой вкладкой настроек.
    doc["pointSec"]    = DEFAULT_POINT_SEC;
    doc["saveMin"]     = saveIntervalMs / 60000;
    doc["minPointSec"] = DEFAULT_POINT_SEC;
    doc["maxPointSec"] = DEFAULT_POINT_SEC;
    doc["minSaveMin"]  = MIN_SAVE_MIN;
    doc["maxSaveMin"]  = MAX_SAVE_MIN;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("pointSec", true)) {
      // Период точки больше не настраивается: точки — пятиминутные средние.
      Serial.println("[CFG] период точки фиксирован (300 с), параметр проигнорирован");
    }
    if (req->hasParam("saveMin", true)) {
      int v          = req->getParam("saveMin", true)->value().toInt();
      v              = constrain(v, MIN_SAVE_MIN, MAX_SAVE_MIN);
      saveIntervalMs = (uint32_t)v * 60000UL;
      prefs.putUInt("smin", (uint32_t)v);
    }
    Serial.printf("[CFG] точка: фиксированно %d с, сохранение: %lu мин\n",
                  DEFAULT_POINT_SEC, saveIntervalMs / 60000);
    req->send(200, "text/plain", "Сохранено");
  });

  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *req) {
    String out;
    if (fsLock(500)) {
      out = listLogFiles();
      fsUnlock();
    } else {
      // Не читаем кэш без блокировки: его может обновлять задача записи.
      req->send(503, "text/plain", "Файловая система занята, повторите");
      return;
    }
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/file", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("name")) {
      req->send(400, "text/plain", "Укажите name");
      return;
    }
    String base = req->getParam("name")->value();
    if (!isSafeLogName(base)) {
      req->send(400, "text/plain", "Некорректное имя файла");
      return;
    }
    // Отдаём файл как есть, без пересборки и добавления заголовков.
    req->send(LittleFS, String(LOGS_DIR) + "/" + base, "text/csv", true);
  });

  server.on("/api/logs/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
    String base;
    if (req->hasParam("name", true))
      base = req->getParam("name", true)->value();
    else if (req->hasParam("name"))
      base = req->getParam("name")->value();
    if (!isSafeLogName(base)) {
      req->send(400, "text/plain", "Некорректное имя файла");
      return;
    }
    String path = String(LOGS_DIR) + "/" + base;
    if (!fsLock()) {
      req->send(503, "text/plain", "Файловая система занята, повторите");
      return;
    }
    if (!LittleFS.exists(path)) {
      fsUnlock();
      req->send(404, "text/plain", "Файл не найден");
      return;
    }
    bool     ext  = isExtLogFile(path);
    uint32_t gone = ext ? 0 : countFileRows(path);
    bool     ok   = LittleFS.remove(path);
    if (ok) {
      cacheRemovePath(path);
      if (!ext) {
        logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
        oldestSavedTs = oldestMainTs();
      }
    }
    fsUnlock();
    if (!ok) {
      req->send(500, "text/plain", "Не удалось удалить файл");
      return;
    }
    Serial.printf("[LOG] удалён по запросу: %s\n", base.c_str());
    req->send(200, "text/plain", "Удалён " + base);
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
    uint16_t co2Now = getLatest().co2;
    if (co2Now)
      doc["co2"] = co2Now;
    else
      doc["co2"] = nullptr;
    doc["mhzOk"]    = mhzOk;
    doc["mhzReady"] = mhzFirstReadDone;
    
    uint32_t heatLeft = heatingLeftSec();
    doc["heating"]  = (heatLeft > 0);
    doc["heatLeft"] = heatLeft;

    char out[224];
    serializeJson(doc, out, sizeof(out));
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!fsLock()) {
      req->send(503, "text/plain", "Файловая система занята, повторите");
      return;
    }
    static String names[MAX_LIST_FILES];
    int           nf = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
    sortNames(names, nf);
    const int perPass = 2;
    int       removed = 0;
    for (int i = 0; i < nf && removed < perPass; i++) {
      bool     ext  = isExtLogFile(names[i]);
      uint32_t gone = ext ? 0 : countFileRows(names[i]);
      if (LittleFS.remove(names[i]))
        cacheRemovePath(names[i]);
      if (!ext)
        logRowsTotal = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
      removed++;
    }
    int left      = nf - removed;
    oldestSavedTs = oldestMainTs();
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

static void startMdns() {
  if (mdnsStarted && !mdnsRestart)
    return;

  static uint32_t lastTry = 0;
  if (!mdnsRestart && lastTry && millis() - lastTry < 10000)
    return;
  lastTry = millis();

  if (mdnsRestart) {
    mdnsRestart = false;
    // Сравнение IP через (uint32_t) ломается между core 2.x и 3.x (разный
    // порядок байт) — из-за этого mDNS не перезапускался после reconnect.
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

  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch ((int)event) {
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.printf("[NET] STA: подключено к \"%s\", канал %u\n",
                      (const char *)info.wifi_sta_connected.ssid,
                      (unsigned)info.wifi_sta_connected.channel);
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        lastStaDiscReason = info.wifi_sta_disconnected.reason;
        Serial.printf("[NET] STA: отключено, reason=%u\n", (unsigned)lastStaDiscReason);
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[NET] STA подключено, IP: %s\n",
                      IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        staDownSinceMs = 0;
        mdnsRestart    = true;
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        Serial.println("[NET] STA: потерян IP");
        mdnsRestart = true;
        break;
      default:
        break;
    }
  });

  // Точка доступа поднимается всегда (резерв для перенастройки), а STA
  // подключается к домашней сети поверх неё (AP+STA). Если связь не поднялась —
  // остаёмся в AP, а netTick() продолжит попытки и переведёт нас в STA, когда
  // сеть появится. Только так устройство не «застревает» в AP и снова видно в сети.
  if (cfgSsid.length()) {
    Serial.println("[NET] Пробую подключиться к: " + cfgSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    // setHostname() строго после softAP(): в core 2.x softAP() сбрасывает
    // hostname STA, заданный раньше.
    WiFi.setHostname(MDNS_HOST);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    // Ждём именно выдачи IP, а не только WL_CONNECTED: бывает CONNECTED с 0.0.0.0.
    uint32_t t0 = millis();
    while (millis() - t0 < 15000) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0))
        break;
      delay(200);
    }
    apMode = !(WiFi.status() == WL_CONNECTED &&
               WiFi.localIP() != IPAddress(0, 0, 0, 0));
    if (apMode)
      Serial.println("[NET] Не удалось получить IP, работаю точкой доступа (STA продолжит переподключение)");
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
  }

  lastNtpTry = 0;
  tryNtp();

  // Точка доступа поднята всегда (AP+STA), поэтому captive-portal DNS активен.
  dnsServer.start(53, "*", WiFi.softAPIP());
  dnsStarted = true;
  startMdns();
  Serial.printf("[NET] AP: %s  softAPIP=%s  mode=%d\n", AP_SSID,
                WiFi.softAPIP().toString().c_str(), (int)WiFi.getMode());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("[BOOT] reset reason=%d\n\n", reason);

  Serial.println("\n=== Climat Monitor ===");

  mhzMutex = xSemaphoreCreateMutex();
  fsMutex  = xSemaphoreCreateMutex();

  prefs.begin("cfg", false);

  {
    // Период точки фиксирован (пятиминутные средние), настройка только периода
    // сохранения в файл. Старое значение "psec" в NVS игнорируем.
    uint32_t sm = prefs.getUInt("smin", DEFAULT_SAVE_MIN);
    if (sm < MIN_SAVE_MIN || sm > MAX_SAVE_MIN)
      sm = DEFAULT_SAVE_MIN;
    saveIntervalMs  = sm * 60000UL;
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS отформатирован");
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  recountLogRows();
  // Один раз на старте строим кэш списка файлов, дальше он обновляется в ОЗУ.
  if (fsLock(2000)) {
    rebuildLogFilesCache();
    fsUnlock();
  }
  preloadLiveFromFlash();
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
  setupEspNow();
  setupWeb();

  readSensors();
  setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
  lastLogMs   = millis();
  lastPointMs = millis();
  Serial.printf("[INIT] пятиминутные средние, сохранение на флеш пачками по %lu мин\n",
                saveIntervalMs / 60000);
  Serial.println("[INIT] готово");
}

// Сеть: автопереподключение STA и периодическое сердцебиение в лог.
// Сердцебиение нужно, чтобы отличать «loop() завис» от «просто нет событий».
static void netTick() {
  uint32_t now = millis();

  if (cfgSsid.length()) {
    bool conn  = (WiFi.status() == WL_CONNECTED);
    bool hasIp = conn && (WiFi.localIP() != IPAddress(0, 0, 0, 0));

    // STA поднялось (в т.ч. позже, после старта в AP) — переходим в сеть.
    if (hasIp && apMode) {
      apMode = false;
      Serial.printf("[NET] STA поднялось, работаю в сети, IP %s\n",
                    WiFi.localIP().toString().c_str());
      mdnsRestart = true;
    }

    if (!hasIp) {
      // «Связи нет» — и когда WL_DISCONNECTED, и когда WL_CONNECTED без IP.
      if (!staDownSinceMs) {
        staDownSinceMs = now;
        Serial.printf("[NET] нет рабочего STA (статус %d, ip %s, причина %u)\n",
                      (int)WiFi.status(), WiFi.localIP().toString().c_str(),
                      (unsigned)lastStaDiscReason);
      }
      // Жёсткий рестарт только после 2 мин простоя и не чаще раза в 30 с.
      // reconnect() каждые 10 с ломал DHCP — IP не запрашивался заново.
      if (now - lastStaRetryMs >= 30000 && now - staDownSinceMs >= 120000) {
        lastStaRetryMs = now;
        Serial.println("[NET] простой STA > 2 мин — перезапускаю подключение");
        WiFi.disconnect(true, false);
        delay(100);
        WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
      }
    } else {
      staDownSinceMs = 0;
    }
  }
  
  if (now - lastHeartbeatMs >= 30000) {
    lastHeartbeatMs = now;
    Serial.printf("[HB] up=%lus wifi=%d ip=%s gw=%s mask=%s dns=%s rssi=%d heap=%u maxAlloc=%u dht=%d mhz=%d heat=%d ap=%d mdns=%d\n",
                  (unsigned long)(now / 1000), (int)WiFi.status(),
                  (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.subnetMask().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(),
                  (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (int)dhtOk, (int)mhzOk, (int)mhzHeating,
                  (int)apMode, (int)mdnsStarted);
  }
}

void loop() {
  if (dnsStarted)
    dnsServer.processNextRequest();

  netTick();
  startMdns();

  // Опрос датчиков каждые SENSOR_PERIOD_MS и накопление пятиминутных средних.
  static uint32_t lastRead = 0;
  if (millis() - lastRead >= SENSOR_PERIOD_MS) {
    lastRead = millis();
    readSensors();
    tryNtp();
    accumulateAvg();
  }

  // Запись на флеш: как только в буфере набралось saveMin минут данных
  // (по 5 мин на точку, то есть saveMin/5 точек), сбрасываем его в файл.
  int flushPts = (int)(saveIntervalMs / 60000UL) / (int)(SAMPLE_PERIOD_SEC / 60UL);
  if (flushPts < 1)
    flushPts = 1;
  if (timeSynced && hourCount >= flushPts)
    flushHourToFlash();

  delay(50);
}

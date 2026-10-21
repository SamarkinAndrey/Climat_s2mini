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
#define NTP_SERVER         "ru.pool.ntp.org"
#define MDNS_HOST          "climat"
// Минутные средние: опрос датчиков каждые 5 с, 12 измерений на минуту.
// Запись на флеш — часовой пачкой (flushHourToFlash), поэтому saveMin задаёт
// только нижнюю границу защиты от частой записи и отображается в настройках.
#define SENSOR_PERIOD_MS   5000UL
#define DEFAULT_POINT_SEC  60
#define DEFAULT_SAVE_MIN   60
#define MIN_POINT_SEC      5
#define MAX_POINT_SEC      600
#define MIN_SAVE_MIN       10
#define MAX_SAVE_MIN       1440
#define MAX_LIVE_POINTS    720
#define MAX_HISTORY_OUT    300
#define MAX_FILE_ROWS      500
#define HISTORY_STEP       5
#define SAMPLES_PER_MIN    12
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
#define MAX_EXT_PTS 720 // ~сутки минутных средних внешнего блока

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

// Аккумуляторы минутных средних (внутренний и внешний блоки)
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
static MinuteAvg    hourBuf[60];
static uint8_t      hourCount = 0;
static ExtAvg extHourBuf[60];
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
#define MAX_LIST_FILES 32

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
  static String names[MAX_LIST_FILES];
  int n = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  uint32_t total = 0;
  for (int i = 0; i < n; i++)
    total += countFileRows(names[i]);
  logRowsTotal = total;

  oldestSavedTs = 0;
  if (n > 0) {
    int oi = 0;
    for (int i = 1; i < n; i++)
      if (names[i] < names[oi])
        oi = i;
    oldestSavedTs = firstFileTs(names[oi]);
  }
  Serial.printf("[LOG] файлов %u, записей всего %lu, самая ранняя %lu\n",
                (unsigned)n, (unsigned long)total, (unsigned long)oldestSavedTs);
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
    LittleFS.remove(names[oldestIdx]);
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
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
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
  portEXIT_CRITICAL(&extMux);

  accExt.t += pkt.temp;
  accExt.h += pkt.hum;
  accExt.p += pkt.press;
  accExt.b += pkt.batt;
  accExt.nExt++;
}

static void setupEspNow() {
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

// Минутные средние: вызывается из loop() каждые SENSOR_PERIOD_MS.
// Раз в SAMPLES_PER_MIN тиков закрывает минуту: среднее идёт в liveBuf/extBuf
// (для графиков и /api/history?since=) и в часовые буферы для записи на флеш.
static void accumulateMinute() {
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

  // внешний блок считаем живым, если пакет был за последние 15 минут
  bool extAlive;
  portENTER_CRITICAL(&extMux);
  extAlive = extSeen && (millis() - extLastRxMs < 15UL * 60UL * 1000UL);
  portEXIT_CRITICAL(&extMux);
  if (!extAlive)
    accExt.nExt = 0;

  if (++accN < SAMPLES_PER_MIN)
    return;

  // ---- минута закрылась ----
  uint32_t ts = timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0;

  if (accIn.nTh || accIn.nCo2) {
    float avgT   = accIn.nTh ? accIn.t / accIn.nTh : r.temperature;
    float avgH   = accIn.nTh ? accIn.h / accIn.nTh : r.humidity;
    uint16_t avgCo2 = accIn.nCo2 ? (uint16_t)(accIn.co2 / accIn.nCo2) : 0;

    Reading avg;
    avg.timestamp   = ts;
    avg.temperature = avgT;
    avg.humidity    = avgH;
    avg.co2         = avgCo2;
    pushLivePoint(avg);

    if (hourCount < 60) {
      hourBuf[hourCount].t   = avgT;
      hourBuf[hourCount].h   = avgH;
      hourBuf[hourCount].co2 = avgCo2;
      hourBuf[hourCount].ts  = ts;
      hourCount++;
    }
  }

  if (accExt.nExt) {
    ExtAvg ea;
    ea.t  = accExt.t / accExt.nExt;
    ea.h  = accExt.h / accExt.nExt;
    ea.p  = accExt.p / accExt.nExt;
    ea.b  = accExt.b / accExt.nExt;
    ea.ts = ts;
    pushExtAvg(ea);
    if (extHourCount < 60) {
      extHourBuf[extHourCount] = ea;
      extHourCount++;
    }
  }

  accIn  = MinAcc();
  accExt = MinAcc();
  accN   = 0;
}

// Запись часовой пачки минутных средних на флеш (ресурс ФС: ~9 строк/сутки).
static void flushHourToFlash() {
  bool locked = fsLock(500);
  if (!locked) {
    Serial.println("[LOG] ФС занята, сброс часа отложен");
    return;
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  if (hourCount > 0) {
    String path  = todayFile();
    bool   isNew = !LittleFS.exists(path);
    File   f     = LittleFS.open(path, "a");
    if (f) {
      if (isNew)
        f.println("timestamp,datetime,temperature,humidity,co2");
      for (uint8_t i = 0; i < hourCount; i++)
        f.printf("%lu,%s,%.2f,%.2f,%u\n", hourBuf[i].ts, fmtTime(hourBuf[i].ts).c_str(),
                 hourBuf[i].t, hourBuf[i].h, hourBuf[i].co2);
      f.close();
      logRowsTotal += hourCount;
      Serial.printf("[LOG] часовой сброс: %u строк в %s\n", hourCount, path.c_str());
      hourCount = 0;
    } else {
      Serial.println("[LOG] ошибка открытия файла, сброс отложен");
    }
  }

  if (extHourCount > 0) {
    String path  = extTodayFile();
    bool   isNew = !LittleFS.exists(path);
    File   f     = LittleFS.open(path, "a");
    if (f) {
      if (isNew)
        f.println("timestamp,datetime,temperature,humidity,pressure,batt");
      for (uint8_t i = 0; i < extHourCount; i++)
        f.printf("%lu,%s,%.2f,%.2f,%.1f,%.2f\n", extHourBuf[i].ts,
                 fmtTime(extHourBuf[i].ts).c_str(), extHourBuf[i].t, extHourBuf[i].h,
                 extHourBuf[i].p, extHourBuf[i].b);
      f.close();
      Serial.printf("[LOG] часовой сброс ext: %u строк в %s\n", extHourCount, path.c_str());
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

  // heating и heatLeft считаем из одного свежего elapsed: mhzHeating обновляется
  // только раз в 5 с в readSensors(), поэтому на границе прогрева heatLeft уходил в мусор.
  uint32_t heatElapsed = millis() - heatingStartMs;
  bool     heatingNow  = (heatElapsed < MHZ19_HEATING_TIME);

  Reading      r     = getLatest();
  doc["temperature"] = r.temperature;
  doc["humidity"]    = r.humidity;
  doc["co2"]         = r.co2;
  doc["dhtOk"]       = dhtOk;
  doc["mhzOk"]       = mhzOk;
  doc["heating"]     = heatingNow;
  doc["heatLeft"]    = heatingNow ? (int)((MHZ19_HEATING_TIME - heatElapsed) / 1000UL) : 0;
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
  doc["frontCap"]     = FRONT_CAP_PERIOD;
  doc["capPeriod"]    = FRONT_CAP_PERIOD;
  doc["capAll"]       = FRONT_CAP_ALL;
  int lvHead, lvCount;
  liveSnapshot(lvHead, lvCount);
  doc["points"] = lvCount;

  // Внешний блок (ESP-NOW): последние минутные средние + статус связи
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

// Читает последние maxRows строк файла в статическое кольцо и возвращает их
// количество; out указывает на g_rows (не использовать повторно до конца запроса).
static HPoint g_rows[MAX_FILE_ROWS];
static HPoint g_ring[MAX_FILE_ROWS];

static int readFileTailStatic(const String &path, int maxRows, HPoint **out) {
  if (maxRows < 1)
    maxRows = 1;
  if (maxRows > MAX_FILE_ROWS)
    maxRows = MAX_FILE_ROWS;
  File f = LittleFS.open(path, "r");
  if (!f) {
    *out = g_rows;
    return 0;
  }

  int   cnt = 0, head = 0;
  char  line[128];
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
    HPoint &p = g_ring[head];
    p.ts      = ts;
    p.t       = atof(c2 + 1);
    p.h       = atof(c3 + 1);
    p.co2     = (uint16_t)atoi(c4 + 1);
    head      = (head + 1) % maxRows;
    if (cnt < maxRows)
      cnt++;
  }
  f.close();

  // разворачиваем кольцо в линейный порядок (старые -> новые)
  int start = (head - cnt + maxRows) % maxRows;
  for (int i = 0; i < cnt; i++)
    g_rows[i] = g_ring[(start + i) % maxRows];
  *out = g_rows;
  return cnt;
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

// Минутные средние внешнего блока (для /api/ext_history): кольцо + снимок.
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
  AsyncResponseStream *resp = req->beginResponseStream("application/json");
  resp->print('[');
  char dt[24];
  for (int i = 0; i < n; i++) {
    if (i)
      resp->print(',');
    fmtTimeBuf(g_extOut[i].ts, dt, sizeof(dt));
    resp->printf("{\"ts\":%lu,\"dt\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"p\":%.1f,\"batt\":%.2f}",
                 (unsigned long)g_extOut[i].ts, dt, g_extOut[i].t, g_extOut[i].h,
                 g_extOut[i].p, g_extOut[i].b);
  }
  resp->print(']');
  req->send(resp);
}

static void sendHistorySince(AsyncWebServerRequest *req, uint32_t since) {
  static HPoint g_sinceOut[MAX_HISTORY_OUT];
  int lh, lc;
  liveSnapshot(lh, lc);
  int start = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;

  // Инкремент: все новые минутные точки без прореживания (обычно 1–5 точек).
  int outCount = 0;
  for (int i = 0; i < lc && outCount < MAX_HISTORY_OUT; i++) {
    Point sp = liveGet((start + i) % MAX_LIVE_POINTS);
    if (sp.ts <= since)
      continue;
    g_sinceOut[outCount++] = HPoint{sp.ts, sp.t, sp.h, sp.co2};
  }

  AsyncResponseStream *resp = req->beginResponseStream("application/json");
  resp->print('[');
  bool firstItem = true;
  for (int i = 0; i < outCount; i++)
    printPoint(*resp, g_sinceOut[i], firstItem);
  resp->print(']');
  req->send(resp);
}

// История без std::vector: всё в статических буферах, между файлами — yield().
// rows[] принимает СНАЧАЛА часовые пачки из liveBuf (новые минуты), затем
// поверх сдвигаются более старые строки из файлов. Файлы читаются от новых
// к старым, каждая строка добавляется в начало массива (сдвиг хвоста),
// поэтому порядок всегда остаётся «старые -> новые».
static void sendHistory(AsyncWebServerRequest *req, uint32_t since, int lastN) {
  static HPoint rows[MAX_FILE_ROWS];
  Serial.printf("[MEM] sendHistory: free heap=%u, largest block=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  int maxOut = (lastN > 0) ? lastN : MAX_HISTORY_OUT;

  int lh, lc;
  liveSnapshot(lh, lc);
  int start = (lh - lc + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;

  // Часовые пачки минутных средних: все точки свежее `since` (или все, если
  // since==0 — режим lastN). newestFirst — индекс кольца самой старой принятой
  // живой точки, oldestLive — её ts.
  int      newestFirst  = 0;
  uint32_t oldestLive   = 0xFFFFFFFFu;
  int      liveInWindow = 0;
  for (int i = lc - 1; i >= 0; i--) {
    Point sp = liveGet((start + i) % MAX_LIVE_POINTS);
    if (sp.ts == 0)
      break; // пустые/несинхронизированные минуты не участвуют
    if (since && sp.ts < since)
      break;
    if (!liveInWindow)
      newestFirst = i; // самая свежая принятая точка
    liveInWindow++;
    oldestLive = sp.ts;
  }

  int nrow = 0;
  // Живые минутные точки в конец выхода (самые новые), от старых к новым.
  if (liveInWindow > maxOut)
    liveInWindow = maxOut; // защита от переполнения (обычно их 1–5)
  for (int i = 0; i < liveInWindow; i++) {
    Point sp     = liveGet((newestFirst - liveInWindow + 1 + i + MAX_LIVE_POINTS * 2) % MAX_LIVE_POINTS);
    rows[nrow++] = HPoint{sp.ts, sp.t, sp.h, sp.co2};
  }

  // Остаток бюджета — строки из файлов (строже: старше oldestLive и свежее since).
  int budget = (lastN > 0) ? (maxOut - nrow) : (MAX_FILE_ROWS - nrow);
  if (budget > 0 && fsLock()) {
    static String names[MAX_LIST_FILES];
    int nf = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
    sortNames(names, nf);

    for (int fi = nf - 1; fi >= 0 && nrow < MAX_FILE_ROWS; fi--) {
      yield();
      HPoint *tail  = nullptr;
      int     cnt   = readFileTailStatic(names[fi], MAX_FILE_ROWS, &tail);
      // строки файла старше живых? берём только ts < oldestLive и ts > since
      for (int i = cnt - 1; i >= 0; i--) {
        const HPoint &p = tail[i];
        if (p.ts == 0)
          continue;
        if (p.ts >= oldestLive)
          continue;
        if (since && p.ts < since)
          break; // файл весь старее окна
        if (nrow >= MAX_FILE_ROWS)
          break;
        // вставляем в начало "файловой" части (перед живыми точками):
        // сдвигаем хвост на 1 и пишем в позицию (nrow - liveInWindow)
        int fpos = nrow - liveInWindow; // сколько файловых строк уже есть
        if (fpos > 0)
          memmove(rows + 1, rows, sizeof(HPoint) * fpos);
        rows[0] = p;
        nrow++;
        if (lastN > 0 && nrow >= maxOut)
          break;
      }
    }
    fsUnlock();
  }

  // Прореживание до maxOut: каждую HISTORY_STEP-ю точку, всегда сохраняя
  // последнюю (самую свежую). Живые точки (последние liveInWindow) не режем.
  AsyncResponseStream *resp = req->beginResponseStream("application/json");
  resp->print('[');
  bool firstItem = true;
  int  fileRows  = nrow - liveInWindow;
  if (fileRows > 0) {
    int fcap = maxOut - liveInWindow;
    if (fcap < 1)
      fcap = 1;
    int step = 1;
    if (fileRows > fcap) {
      step = (fileRows + fcap - 1) / fcap;
      if (step < HISTORY_STEP)
        step = HISTORY_STEP;
    }
    for (int i = 0; i < fileRows; i += step)
      printPoint(*resp, rows[i], firstItem);
  }
  for (int i = 0; i < liveInWindow; i++) {
    // если живых больше бюджета — тоже прореживаем, кроме последней
    if (liveInWindow > maxOut && i % HISTORY_STEP != 0 && i != liveInWindow - 1)
      continue;
    printPoint(*resp, rows[fileRows + i], firstItem);
  }
  resp->print(']');
  req->send(resp);
}

static String listLogFiles() {
  static String names[MAX_LIST_FILES];
  String        out = "[";
  int           nf  = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
  sortNames(names, nf);

  for (int i = 0; i < nf; i++) {
    int    slash = names[i].lastIndexOf('/');
    String base  = slash >= 0 ? names[i].substring(slash + 1) : names[i];
    if (i)
      out += ",";
    out += "\"" + base + "\"";
  }
  out += "]";
  return out;
}

extern const uint8_t        index_html_start[] asm("_binary_firmware_internal_index_html_start");
extern const uint8_t        index_html_end[] asm("_binary_firmware_internal_index_html_end");
static const uint8_t *const INDEX_HTML     = index_html_start;
static const size_t         INDEX_HTML_LEN = (size_t)(index_html_end - index_html_start);

struct ExportCtx {
  String names[MAX_LIST_FILES];
  int    n = 0;
  int    idx = 0;
  File   f;
  size_t headerPos = 0;
  char   pending[168];
  size_t pendingLen = 0;
};

static bool exportNextLine(ExportCtx &c, char *line, size_t n) {
  while (c.idx < c.n) {
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
    static ExportCtx ctx; // статический — не фрагментируем кучу на каждый экспорт
    ctx     = ExportCtx();
    if (fsLock(2000)) {
      ctx.n = listFiles(LOGS_DIR, ctx.names, MAX_LIST_FILES);
      fsUnlock();
    }
    sortNames(ctx.names, ctx.n);
    ExportCtx *pc = &ctx;
    req->sendChunked("text/csv", [pc](uint8_t *buf, size_t maxLen, size_t) -> size_t {
      return exportFill(pc, buf, maxLen);
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
    static String names[MAX_LIST_FILES];
    int           nf = listFiles(LOGS_DIR, names, MAX_LIST_FILES);
    sortNames(names, nf);
    const int perPass = 2;
    int       removed = 0;
    for (int i = 0; i < nf && removed < perPass; i++) {
      uint32_t gone = countFileRows(names[i]);
      logRowsTotal  = (logRowsTotal > gone) ? (logRowsTotal - gone) : 0;
      LittleFS.remove(names[i]);
      removed++;
    }
    int left      = nf - removed;
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

  WiFi.setAutoReconnect(true);
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
  
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("[BOOT] reset reason=%d\n\n", reason);

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
  setupEspNow();
  setupWeb();

  readSensors();
  setLatestTs(timeSynced && (time(nullptr) > 1000000) ? (uint32_t)time(nullptr) : 0);
  lastLogMs   = millis();
  lastPointMs = millis();
  Serial.printf("[INIT] минутные средние, сохранение на флеш пачками по %lu мин\n",
                saveIntervalMs / 60000);
  Serial.println("[INIT] готово");
}

// Сеть: автопереподключение STA и периодическое сердцебиение в лог.
// Сердцебиение нужно, чтобы отличать «loop() завис» от «просто нет событий».
static void netTick() {
  uint32_t now = millis();

  if (cfgSsid.length()) {
    bool conn = (WiFi.status() == WL_CONNECTED);

    if (conn && apMode) {
      apMode = false;
      Serial.printf("[NET] STA поднялось, работаю в сети, IP %s\n",
                    WiFi.localIP().toString().c_str());
      mdnsRestart = true;
    }

    if (!conn) {
      if (!staDownSinceMs) {
        staDownSinceMs = now;
        Serial.printf("[NET] нет STA-связи (статус %d, последняя причина %u)\n",
                      (int)WiFi.status(), (unsigned)lastStaDiscReason);
      }
      if (now - lastStaRetryMs >= 10000) {
        lastStaRetryMs = now;
        if (now - staDownSinceMs >= 120000) {
          Serial.println("[NET] простой STA > 2 мин — перезапускаю подключение");
          WiFi.disconnect(false, false);
          WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
        } else {
          Serial.printf("[NET] попытка переподключения STA (статус %d)\n", (int)WiFi.status());
          WiFi.reconnect();
        }
      }
    } else {
      staDownSinceMs = 0;
    }
  }

  if (now - lastHeartbeatMs >= 30000) {
    lastHeartbeatMs = now;
    Serial.printf("[HB] up=%lus wifi=%d ip=%s gw=%s mask=%s dns=%s rssi=%d heap=%u dht=%d mhz=%d heat=%d ap=%d mdns=%d\n",
                  (unsigned long)(now / 1000), (int)WiFi.status(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.subnetMask().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(),
                  (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0,
                  (unsigned)ESP.getFreeHeap(), (int)dhtOk, (int)mhzOk, (int)mhzHeating,
                  (int)apMode, (int)mdnsStarted);
  }
}

void loop() {
  if (dnsStarted)
    dnsServer.processNextRequest();

  netTick();
  startMdns();

  // Опрос датчиков каждые SENSOR_PERIOD_MS и накопление минутных средних.
  static uint32_t lastRead = 0;
  if (millis() - lastRead >= SENSOR_PERIOD_MS) {
    lastRead = millis();
    readSensors();
    tryNtp();
    accumulateMinute();
  }

  // Запись на флеш — часовой пачкой: как только в часовом буфере набралось
  // DEFAULT_SAVE_MIN минут (по умолчанию 60), сбрасываем его в файл.
  if (timeSynced && hourCount >= (int)(saveIntervalMs / 60000UL))
    flushHourToFlash();

  delay(50);
}

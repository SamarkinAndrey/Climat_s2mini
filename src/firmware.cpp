#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
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

#define DEFAULT_POINT_SEC 30
#define DEFAULT_SAVE_MIN  5
#define MIN_POINT_SEC     5
#define MAX_POINT_SEC     600
#define MIN_SAVE_MIN      1
#define MAX_SAVE_MIN      1440
#define MAX_LIVE_POINTS   1440
#define MAX_HISTORY_OUT   800

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
uint32_t heatingStartMs  = 0;      // момент старта датчика (для отсчёта прогрева)
uint32_t lastLogMs       = 0;
uint32_t lastNtpTry      = 0;
uint32_t lastPointMs     = 0;
bool     apMode          = true;
bool     timeSynced      = false;
uint32_t pointIntervalMs = DEFAULT_POINT_SEC * 1000UL;
uint32_t saveIntervalMs  = DEFAULT_SAVE_MIN * 60UL * 1000UL;
uint32_t pointsSeq       = 0;

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

static String fmtTime(uint32_t ts) {
  struct tm t;
  time_t    tt = ts;
  localtime_r(&tt, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
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

static void rotateLogsIfNeeded() {
  std::vector<String> names;
  listFiles(LOGS_DIR, names);

  while (names.size() > MAX_LOG_FILES) {
    int oldestIdx = 0;
    for (size_t i = 1; i < names.size(); i++)
      if (names[i] < names[oldestIdx])
        oldestIdx = i;
    Serial.printf("[LOG] ротация: удаляю %s\n", names[oldestIdx].c_str());
    LittleFS.remove(names[oldestIdx]);
    names.erase(names.begin() + oldestIdx);
  }
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
  if (timeSynced && time(nullptr) > 1000000) {
    doc["datetime"] = fmtTime(latest.timestamp);
  } else {
    doc["datetime"] = "NTP ожидание...";
  }
  doc["uptime"]       = millis() / 1000;
  doc["mode"]         = apMode ? "AP" : "STA";
  doc["ip"]           = WiFi.localIP().toString();
  doc["nextLogInSec"] = (timeSynced && time(nullptr) > 1000000)
                            ? ((lastLogMs + saveIntervalMs > millis())
                                   ? (lastLogMs + saveIntervalMs - millis()) / 1000
                                   : 0)
                            : 0;
  doc["pointSec"]     = pointIntervalMs / 1000;
  doc["saveMin"]      = saveIntervalMs / 60000;
  doc["points"]       = liveCount;
  doc["pointsSeq"]    = pointsSeq;
  String out;
  serializeJson(doc, out);
  return out;
}

static String jsonHistory(int points) {
  if (points < 1)
    points = 1;
  if (points > 20000)
    points = 20000;

  std::vector<Point> live;
  live.reserve(liveCount);
  int first = (liveHead - liveCount + MAX_LIVE_POINTS) % MAX_LIVE_POINTS;
  for (int i = 0; i < liveCount; i++)
    live.push_back(liveBuf[(first + i) % MAX_LIVE_POINTS]);
  uint32_t oldestLive = live.empty() ? 0xFFFFFFFFu : live.front().ts;

  int fromFile = points - (int)live.size();
  if (fromFile < 0) {
    live.erase(live.begin(), live.begin() + ((int)live.size() - points));
    fromFile = 0;
  }
  if (fromFile > MAX_HISTORY_OUT)
    fromFile = MAX_HISTORY_OUT;

  std::vector<String> rows;
  if (fromFile > 0) {
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i])
          std::swap(names[i], names[j]);

    for (int fi = (int)names.size() - 1; fi >= 0 && (int)rows.size() < fromFile; fi--) {
      File f = LittleFS.open(names[fi], "r");
      if (!f)
        continue;
      std::vector<String> fileRows;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() && line.indexOf(',') > 0 && !line.startsWith("timestamp"))
          fileRows.push_back(line);
      }
      f.close();

      for (int i = (int)fileRows.size() - 1; i >= 0 && (int)rows.size() < fromFile; i--) {
        int c = fileRows[i].indexOf(',');
        if (c <= 0)
          continue;
        uint32_t ts = (uint32_t)fileRows[i].substring(0, c).toInt();
        if (ts == 0 || ts >= oldestLive)
          continue;
        rows.push_back(fileRows[i]);
      }
    }
    std::reverse(rows.begin(), rows.end());
  }

  size_t total = rows.size() + live.size();
  int    step  = (total > MAX_HISTORY_OUT) ? (int)((total + MAX_HISTORY_OUT - 1) / MAX_HISTORY_OUT) : 1;

  JsonDocument doc;
  JsonArray    arr = doc.to<JsonArray>();

  int off = (total > 0) ? ((int)total - 1) % step : 0;
  for (int i = off; i < (int)total; i += step) {
    JsonObject o = arr.createNestedObject();
    if (i < (int)rows.size()) {
      const String &ln = rows[i];
      int           p1 = ln.indexOf(',');
      int           p2 = ln.indexOf(',', p1 + 1);
      int           p3 = ln.indexOf(',', p2 + 1);
      int           p4 = ln.indexOf(',', p3 + 1);
      uint32_t ts      = (uint32_t)ln.substring(0, p1).toInt();
      o["ts"]          = ts;
      o["dt"]          = (ts > 1000000) ? fmtTime(ts) : ln.substring(p1 + 1, p2);
      o["t"]           = ln.substring(p2 + 1, p3).toFloat();
      o["h"]           = ln.substring(p3 + 1, p4).toFloat();
      o["co2"]         = ln.substring(p4 + 1).toInt();
    } else {
      const Point &p = live[i - rows.size()];
      o["ts"]        = p.ts;
      o["dt"]        = fmtTime(p.ts);
      o["t"]         = p.t;
      o["h"]         = p.h;
      o["co2"]       = p.co2;
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
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
#v-co2.heat{color:var(--bad);font-size:22px}   /* слово «нагрев» вместо цифр */
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
    <span class="badge" id="timeBadge">—</span>
    <span class="badge" id="logBadge">—</span>
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
    <div class="prow pbtns">
      <button class="btn sec" onclick="exportCsv()">Скачать CSV</button>
    </div>
  </div>
</div>

<div class="tab" id="tab-wifi">
  <div class="panel">
    <h2>Подключение к домашней сети (STA)</h2>
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px">После подключения устройство перезагрузится и выйдет в интернет через роутер. Точка доступа <b>CO2Monitor</b> остаётся резервным режимом, если сеть недоступна.<br>Если сохранённой сети нет (сразу после прошивки), устройство само попробует подключиться к сети по умолчанию.</p>
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
      <button class="btn warn" onclick="clearLogs()">Удалить все логи</button>
    </div>
  </div>
  <div class="panel"><h2>Последние записи</h2><div style="overflow-x:auto"><table id="tbl"><thead><tr><th>Дата/время</th><th>T °C</th><th>RH %</th><th>CO₂ ppm</th></tr></thead><tbody></tbody></table></div></div>
</div>

</main>
<script>
let hist=[];
let lastSeq=-1;  
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
function setRange(){ saveUi(); loadCharts(); }

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
 if(openCharts[k]){drawOne(k);loadCharts();}  
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
  const f=String(data[0].dt||'').slice(5,16), l=String(data[data.length-1].dt||'').slice(5,16);
  g.fillText(f,pad.l,h-8);
  g.fillText(l,w-pad.r-g.measureText(l).width,h-8);
  g.beginPath(); g.strokeStyle=color; g.lineWidth=2;
  data.forEach((d,i)=>{
   const x=pad.l+(data.length>1?pw*i/(data.length-1):pw/2);
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
   $('v-co2').innerHTML='нагрев';
   $('s-mhz').innerHTML='<span class="heat">идёт прогрев, '+(s.heatLeft||0)+' с</span>';
  }else{
   $('v-co2').innerHTML=s.co2;
   $('s-mhz').innerHTML=s.mhzOk?'<span class="ok"></span>':'<span class="bad">MH-Z19 нет связи</span>';
  }
  $('s-dht').innerHTML=s.dhtOk?'<span class="ok"></span>':'<span class="bad">DHT22 нет связи</span>';
  $('netBadge').textContent=(s.mode==='AP'?'Точка доступа':'Сеть: '+s.ip);
  $('timeBadge').textContent='Дата: '+s.datetime;
 
  if(s.pointsSeq!==lastSeq){ lastSeq=s.pointsSeq; loadCharts(); }
 }catch(e){}
 setTimeout(pollLive,5000);}

let chartsTimer=null;
async function loadCharts(){
 try{
  hist=await api('/api/history?hours='+$('rangeSel').value);
  drawAll();
  let span='';
  if(hist.length>1){
   const t0=hist[0].ts, t1=hist[hist.length-1].ts;
   const hours=Math.round((t1-t0)/3600);
   span=hours>=24?(' · '+Math.round(hours/24)+' сут'):(' · '+hours+' ч');
  }
  $('logBadge').textContent='Точек: '+hist.length+span;
 }catch(e){}}

function startChartsLive(){ if(chartsTimer)clearInterval(chartsTimer); chartsTimer=setInterval(loadCharts,120000); }

async function loadData(){
 try{
  const files=await api('/api/files');
  $('files').innerHTML=files.length?('Файлов: '+files.length+'<br>'+files.join('<br>')):'Нет файлов';
  const h=await api('/api/history?points=50');
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
  $('calstat').innerHTML=`Диапазон: ${s.range} ppm · ABC: ${s.abc===1||s.abc===true?'вкл':'выкл'} · CO₂ сейчас: ${s.heating?'нагрев':s.co2+' ppm'} · Связь: ${s.heating?'прогрев':(s.mhzOk?'OK':'нет данных')}`;
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
 $('setmsg').textContent=await r.text(); loadCharts();}

function exportCsv(){location.href='/api/export';}
window.addEventListener('resize',drawAll);
loadUi();
pollLive();
loadCharts();
startChartsLive();
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
    int points = 720;
    if (req->hasParam("hours")) {
      int hours = req->getParam("hours")->value().toInt();
      if (hours <= 0) {
        points = 20000;
      } else {
        uint32_t sec = (uint32_t)hours * 3600UL;
        points       = (int)(sec / (pointIntervalMs / 1000UL));
        if (points < 10)
          points = 10;
      }
    } else if (req->hasParam("points")) {
      points = req->getParam("points")->value().toInt();
    }
    points = constrain(points, 10, 20000);
    req->send(200, "application/json", jsonHistory(points));
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
    req->send(200, "text/plain", "Логи удалены");
  });

  server.begin();
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
    }
  });

  if (cfgSsid.length()) {
    Serial.println("[NET] Пробую подключиться к: " + cfgSsid);
    WiFi.mode(WIFI_AP_STA);
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
  if (apMode) {
    Serial.printf("[NET] AP: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Climat Monitor ===");

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

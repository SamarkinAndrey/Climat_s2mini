// ============================================================
//  Прошивка для ESP32-S2 mini (WEMOS LOLIN S2 MINI)
//  DHT22 (t/rH) + MH-Z19B (CO2), сохранение каждые 10 мин,
//  калибровка MH-Z19, AP/STA режимы, веб-интерфейс с графиками.
//
//  Подключение:
//    DHT22 DATA -> GPIO 4  (+ подтяжка к 3.3V)
//    MH-Z19 RX  -> GPIO 16 (TX ESP32)
//    MH-Z19 TX  -> GPIO 17 (RX ESP32)
//    MH-Z19 Vin -> 5V (VBUS), GND -> GND
//
//  Сборка: pio run -e lolin_s2_mini -t upload
//  Веб-страница: http://192.168.4.1 (AP) или IP из роутера (STA)
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <MHZ19.h>
#include <Preferences.h>
#include <WiFi.h>
#include <dhtnew.h>
#include <SoftwareSerial.h>

// ----------------------------- НАСТРОЙКИ -----------------------------
#define DHT_PIN 4        // DHT22 data pin
#define MHZ_RX 17        // MH-Z19 TX -> GPIO17
#define MHZ_TX 16        // MH-Z19 RX <- GPIO16
#define LOG_INTERVAL_MS (10UL * 60UL * 1000UL)  // 10 минут
#define LOGS_DIR "/logs"
#define MAX_LOG_FILES 20                         // ротация старых файлов
#define NTP_SERVER "ru.pool.ntp.org"

const char *AP_SSID = "CO2Monitor";
const char *AP_PASS = "12345678";  // минимум 8 символов

// ----------------------------- ОБЪЕКТЫ -----------------------------
DHTNEW dht(DHT_PIN);
SoftwareSerial mhzSerial(MHZ_RX, MHZ_TX);
MHZ19 mhz;
Preferences prefs;
AsyncWebServer server(80);
DNSServer dnsServer;

// ----------------------------- СОСТОЯНИЕ -----------------------------
struct Reading {
  uint32_t timestamp = 0;  // unix time (если есть NTP), иначе millis/1000
  float temperature = 0;
  float humidity = 0;
  uint16_t co2 = 0;
};

Reading latest;
bool dhtOk = false, mhzOk = false;
uint32_t lastLogMs = 0;
uint32_t lastNtpTry = 0;
bool apMode = true;   // текущий режим сети
bool timeSynced = false;

String cfgSsid, cfgPass;

// ----------------------------- УТИЛИТЫ -----------------------------
static String fmtTime(uint32_t ts) {
  struct tm t;
  time_t tt = ts;
  localtime_r(&tt, &t);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &t);
  return String(buf);
}

static String todayFile() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char name[32];
  sprintf(name, "%s/%04d-%02d-%02d.csv", LOGS_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(name);
}

// ----------------------------- ЛОГИРОВАНИЕ -----------------------------
// Собирает список файлов в директории (имена с полным путём)
static void listFiles(const char *dirPath, std::vector<String> &out) {
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) out.push_back(String(entry.name()));
    entry.close();
  }
  dir.close();
}

static void rotateLogsIfNeeded() {
  std::vector<String> names;
  listFiles(LOGS_DIR, names);

  // имена вида /logs/YYYY-MM-DD.csv сортируются лексикографически по дате
  while (names.size() > MAX_LOG_FILES) {
    int oldestIdx = 0;
    for (size_t i = 1; i < names.size(); i++)
      if (names[i] < names[oldestIdx]) oldestIdx = i;
    Serial.printf("[LOG] ротация: удаляю %s\n", names[oldestIdx].c_str());
    LittleFS.remove(names[oldestIdx]);
    names.erase(names.begin() + oldestIdx);
  }
}

static void logReading(const Reading &r) {
  if (!LittleFS.exists(LOGS_DIR)) LittleFS.mkdir(LOGS_DIR);

  String path = todayFile();
  bool isNew = !LittleFS.exists(path);

  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.println("[LOG] ошибка открытия файла");
    return;
  }
  if (isNew) f.println("timestamp,datetime,temperature,humidity,co2");
  f.printf("%lu,%s,%.2f,%.2f,%u\n", r.timestamp, fmtTime(r.timestamp).c_str(),
           r.temperature, r.humidity, r.co2);
  f.close();
  Serial.printf("[LOG] записано в %s: %.1fC %.1f%% %uppm\n",
                path.c_str(), r.temperature, r.humidity, r.co2);
  rotateLogsIfNeeded();
}

// ----------------------------- ДАТЧИКИ -----------------------------
static void readSensors() {
  int status = dht.read();
  dhtOk = (status == DHTLIB_OK);
  if (dhtOk) {
    latest.temperature = dht.getTemperature();
    latest.humidity = dht.getHumidity();
  }

  int co2 = mhz.getCO2(false, true);
  if (co2 > 0 && co2 <= mhz.getRange()) {
    latest.co2 = co2;
    mhzOk = true;
  } else {
    mhzOk = false;
  }

  latest.timestamp = timeSynced ? (uint32_t)time(nullptr) : (uint32_t)(millis() / 1000);
}

// ----------------------------- NTP -----------------------------
static void tryNtp() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastNtpTry < 300000) return;  // раз в 5 минут
  lastNtpTry = now;
  configTime(3 * 3600, 0, NTP_SERVER);  // MSK +3
  timeSynced = true;
}

// ----------------------------- WEB: JSON -----------------------------
static String jsonCurrent() {
  JsonDocument doc;
  doc["temperature"] = latest.temperature;
  doc["humidity"] = latest.humidity;
  doc["co2"] = latest.co2;
  doc["dhtOk"] = dhtOk;
  doc["mhzOk"] = mhzOk;
  doc["timestamp"] = latest.timestamp;
  doc["datetime"] = fmtTime(latest.timestamp);
  doc["uptime"] = millis() / 1000;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["ip"] = WiFi.localIP().toString();
  doc["nextLogInSec"] = (lastLogMs + LOG_INTERVAL_MS > millis())
                            ? (lastLogMs + LOG_INTERVAL_MS - millis()) / 1000
                            : 0;
  String out;
  serializeJson(doc, out);
  return out;
}

static String jsonHistory(int points) {
  // последние N точек из последних файлов
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  std::vector<String> names;
  listFiles(LOGS_DIR, names);
  // сортировка имён (даты) по возрастанию
  for (size_t i = 0; i < names.size(); i++)
    for (size_t j = i + 1; j < names.size(); j++)
      if (names[j] < names[i]) std::swap(names[i], names[j]);

  // читаем файлы справа налево, пока не наберём points точек
  std::vector<String> rows;
  for (int fi = (int)names.size() - 1; fi >= 0 && (int)rows.size() < points; fi--) {
    File f = LittleFS.open(names[fi], "r");
    if (!f) continue;
    std::vector<String> fileRows;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() && line.indexOf(',') > 0 && !line.startsWith("timestamp"))
        fileRows.push_back(line);
    }
    f.close();
    int need = points - rows.size();
    int start = max(0, (int)fileRows.size() - need);
    for (int i = start; i < (int)fileRows.size(); i++) rows.push_back(fileRows[i]);
  }

  for (size_t i = 0; i < rows.size(); i++) {
    // формат: ts,datetime,t,h,co2
    const String &ln = rows[i];
    int p1 = ln.indexOf(',');
    int p2 = ln.indexOf(',', p1 + 1);
    int p3 = ln.indexOf(',', p2 + 1);
    int p4 = ln.indexOf(',', p3 + 1);
    JsonObject o = arr.createNestedObject();
    o["ts"] = ln.substring(0, p1).toInt();
    o["dt"] = ln.substring(p1 + 1, p2);
    o["t"] = ln.substring(p2 + 1, p3).toFloat();
    o["h"] = ln.substring(p3 + 1, p4).toFloat();
    o["co2"] = ln.substring(p4 + 1).toInt();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

static String listLogFiles() {
  String out = "[";
  std::vector<String> names;
  listFiles(LOGS_DIR, names);
  const int prefixLen = strlen(LOGS_DIR) + 1;
  for (size_t i = 0; i < names.size(); i++) {
    if (i) out += ",";
    out += "\"" + names[i].substring(prefixLen) + "\"";
  }
  out += "]";
  return out;
}

// ----------------------------- HTML (веб-интерфейс) -----------------------------
// Chart.js подключается с CDN; при работе без интернета графики используют
// данные из /api/history, но библиотека может не загрузиться — тогда
// показывается таблица последних значений.
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CO2 Monitor · ESP32-S2</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js"></script>
<style>
:root{--bg:#0f172a;--card:#1e293b;--txt:#e2e8f0;--mut:#94a3b8;--acc:#38bdf8;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh}
header{padding:16px 24px;background:var(--card);display:flex;align-items:center;gap:12px;flex-wrap:wrap}
header h1{font-size:18px;font-weight:600}
.badge{font-size:12px;padding:3px 10px;border-radius:999px;background:#334155;color:var(--mut)}
nav{display:flex;gap:4px;padding:8px 24px;background:var(--card);border-bottom:1px solid #334155}
nav button{background:none;border:none;color:var(--mut);padding:8px 16px;cursor:pointer;font-size:14px;border-radius:8px}
nav button.active{background:var(--acc);color:#04263a;font-weight:600}
main{padding:24px;max-width:1100px;margin:0 auto}
.tab{display:none}.tab.active{display:block}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;margin-bottom:24px}
.card{background:var(--card);border-radius:16px;padding:20px}
.card .label{font-size:13px;color:var(--mut);margin-bottom:6px}
.card .value{font-size:34px;font-weight:700}
.card .unit{font-size:14px;color:var(--mut);font-weight:400}
.card .status{font-size:12px;margin-top:6px}
.ok{color:var(--ok)}.bad{color:var(--bad)}
.panel{background:var(--card);border-radius:16px;padding:20px;margin-bottom:16px}
.panel h2{font-size:15px;margin-bottom:12px;color:var(--acc)}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
input[type=text],input[type=password]{background:#0f172a;border:1px solid #334155;color:var(--txt);padding:9px 12px;border-radius:8px;font-size:14px}
button.btn{background:var(--acc);border:none;color:#04263a;padding:9px 16px;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer}
button.btn.sec{background:#334155;color:var(--txt)}
button.btn.warn{background:var(--warn);color:#241503}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:7px 10px;text-align:left;border-bottom:1px solid #334155;color:var(--mut)}
th{color:var(--txt)}
canvas{max-height:340px}
select{background:#0f172a;border:1px solid #334155;color:var(--txt);padding:8px;border-radius:8px}
#msg{font-size:13px;color:var(--ok);min-height:18px;margin-top:8px}
.gridchart{display:grid;grid-template-columns:1fr;gap:20px}
@media(min-width:900px){.gridchart{grid-template-columns:1fr 1fr}}
.full{grid-column:1/-1}
</style>
</head>
<body>
<header>
  <h1>🌿 CO2 Monitor · ESP32-S2 mini</h1>
  <span class="badge" id="netBadge">—</span>
  <span class="badge" id="timeBadge">—</span>
  <span class="badge" id="logBadge">—</span>
</header>
<nav>
  <button class="active" data-tab="live" onclick="showTab('live')">Реальное время</button>
  <button data-tab="charts" onclick="showTab('charts')">Графики</button>
  <button data-tab="wifi" onclick="showTab('wifi')">Wi-Fi</button>
  <button data-tab="calib" onclick="showTab('calib')">Калибровка MH-Z19</button>
  <button data-tab="data" onclick="showTab('data')">Данные</button>
</nav>
<main>

<div class="tab active" id="tab-live">
  <div class="cards">
    <div class="card"><div class="label">Температура</div><div class="value" id="v-t">—<span class="unit"> °C</span></div><div class="status" id="s-dht"></div></div>
    <div class="card"><div class="label">Влажность</div><div class="value" id="v-h">—<span class="unit"> %</span></div></div>
    <div class="card"><div class="label">CO₂</div><div class="value" id="v-co2">—<span class="unit"> ppm</span></div><div class="status" id="s-mhz"></div></div>
    <div class="card"><div class="label">Следующая запись</div><div class="value" id="v-next">—<span class="unit"> c</span></div><div class="status">каждые 10 минут</div></div>
  </div>
  <div class="panel"><h2>Последние 12 часов (обновление live)</h2><canvas id="liveChart"></canvas></div>
</div>

<div class="tab" id="tab-charts">
  <div class="row" style="margin-bottom:16px">
    <label>Период:</label>
    <select id="rangeSel" onchange="loadCharts()">
      <option value="144">12 часов</option>
      <option value="288">2 суток</option>
      <option value="720" selected>5 суток</option>
      <option value="2000">Максимум (~14 суток)</option>
    </select>
    <button class="btn sec" onclick="loadCharts()">Обновить</button>
    <button class="btn sec" onclick="exportCsv()">Скачать CSV</button>
  </div>
  <div class="gridchart">
    <div class="panel full"><h2>CO₂, ppm</h2><canvas id="chCo2"></canvas></div>
    <div class="panel"><h2>Температура, °C</h2><canvas id="chT"></canvas></div>
    <div class="panel"><h2>Влажность, %</h2><canvas id="chH"></canvas></div>
  </div>
</div>

<div class="tab" id="tab-wifi">
  <div class="panel">
    <h2>Подключение к домашней сети (STA)</h2>
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px">После подключения устройство перезагрузится и выйдет в интернет через роутер. Точка доступа <b>CO2Monitor</b> остаётся резервным режимом, если сеть недоступна.</p>
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
    <p style="color:var(--mut);font-size:13px;margin-bottom:12px"><b>Zero calibration (400 ppm)</b> — вынесите датчик на свежий воздух на 20 минут, затем нажмите. Требуется прогреддатчика ≥ 48 часов.<br><b>Span calibration (2000 ppm)</b> — с эталонным газом 2000 ppm.</p>
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
let charts={}, hist=[], liveData=[];
const $=id=>document.getElementById(id);
function showTab(t){document.querySelectorAll('.tab').forEach(e=>e.classList.remove('active'));
$('tab-'+t).classList.add('active');
document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('active',b.dataset.tab===t));
if(t==='charts')loadCharts(); if(t==='data')loadData(); if(t==='calib')loadStatus();}

function mkChart(id,color,label,data,yfield){
 const ctx=$(id).getContext('2d');
 if(charts[id])charts[id].destroy();
 charts[id]=new Chart(ctx,{type:'line',data:{labels:data.map(d=>d.dt.slice(5)),
 datasets:[{label:label,data:data.map(d=>d[yfield]),borderColor:color,backgroundColor:color+'22',
 tension:.25,pointRadius:0,fill:true}]},
 options:{responsive:true,animation:false,scales:{x:{ticks:{maxTicksLimit:12,color:'#94a3b8'},grid:{color:'#334155'}},
 y:{ticks:{color:'#94a3b8'},grid:{color:'#334155'}}},plugins:{legend:{display:false}}}});}

async function api(u){const r=await fetch(u);return r.json();}

async function pollLive(){
 try{
  const s=await api('/api/current');
  $('v-t').innerHTML=s.temperature.toFixed(1)+'<span class="unit"> °C</span>';
  $('v-h').innerHTML=s.humidity.toFixed(1)+'<span class="unit"> %</span>';
  $('v-co2').innerHTML=s.co2+'<span class="unit"> ppm</span>';
  $('s-dht').innerHTML=s.dhtOk?'<span class="ok">DHT22 OK</span>':'<span class="bad">DHT22 нет связи</span>';
  $('s-mhz').innerHTML=s.mhzOk?'<span class="ok">MH-Z19 OK</span>':'<span class="bad">MH-Z19 нет связи</span>';
  $('v-next').innerHTML=s.nextLogInSec+'<span class="unit"> c</span>';
  $('netBadge').textContent=(s.mode==='AP'?'Точка доступа':'Сеть: '+s.ip);
  $('timeBadge').textContent='Показание: '+s.datetime;
  liveData.push({dt:s.datetime.slice(11),t:s.temperature,h:s.humidity,co2:s.co2});
  if(liveData.length>200)liveData.shift();
  if(window.Chart&&$('tab-live').classList.contains('active'))
    mkChart('liveChart','#38bdf8','CO2 live',[...hist.slice(-72),...liveData],'co2');
 }catch(e){}
 setTimeout(pollLive,5000);}

async function loadCharts(){
 try{
  hist=await api('/api/history?points='+$('rangeSel').value);
  if(!window.Chart){$('tab-charts').insertAdjacentHTML('afterend','');alert('Chart.js не загружен (нет интернета). Данные доступны как CSV.');}
  mkChart('chCo2','#22c55e','CO₂ ppm',hist,'co2');
  mkChart('chT','#f59e0b','Температура',hist,'t');
  mkChart('chH','#38bdf8','Влажность',hist,'h');
  $('logBadge').textContent='Точек: '+hist.length;
 }catch(e){}}

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
  $('calstat').innerHTML=`Диапазон: ${s.range} ppm · ABC: ${s.abc===1||s.abc===true?'вкл':'выкл'} · CO₂ сейчас: ${s.co2} ppm · Связь: ${s.mhzOk?'OK':'нет данных'}`;
 }catch(e){$('calstat').textContent='Ошибка запроса';}}

async function clearLogs(){
 if(!confirm('Удалить все файлы логов?'))return;
 const r=await fetch('/api/logs/clear',{method:'POST'});
 alert(await r.text());loadData();}

function exportCsv(){location.href='/api/export';}
pollLive();
</script>
</body>
</html>)rawliteral";

// ----------------------------- WEB СЕРВЕР -----------------------------
static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/current", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", jsonCurrent());
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    int points = 720;
    if (req->hasParam("points")) points = req->getParam("points")->value().toInt();
    points = constrain(points, 10, 2000);
    req->send(200, "application/json", jsonHistory(points));
  });

  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", listLogFiles());
  });

  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *req) {
    // объединяем все csv в один ответ
    String all = "timestamp,datetime,temperature,humidity,co2\n";
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (size_t i = 0; i < names.size(); i++)
      for (size_t j = i + 1; j < names.size(); j++)
        if (names[j] < names[i]) std::swap(names[i], names[j]);
    for (auto &n : names) {
      File f = LittleFS.open(n, "r");
      if (!f) continue;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (!line.startsWith("timestamp")) all += line + "\n";
      }
      f.close();
    }
    req->send(200, "text/csv", all);
  });

  server.on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    String ssid, pass;
    if (req->hasParam("ssid", true)) ssid = req->getParam("ssid", true)->value();
    if (req->hasParam("pass", true)) pass = req->getParam("pass", true)->value();
    if (ssid.length()) {
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      req->send(200, "text/plain", "Сохранено. Перезагрузка в STA-режим...");
      delay(1500);
      ESP.restart();
    } else {
      req->send(400, "text/plain", "Укажите SSID");
    }
  });

  server.on("/api/wifi/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    prefs.clear();
    req->send(200, "text/plain", "Настройки стёрты. Возврат в AP...");
    delay(1500);
    ESP.restart();
  });

  // -------- КАЛИБРОВКА MH-Z19 --------
  server.on("/api/calibrate", HTTP_GET, [](AsyncWebServerRequest *req) {
    String type = "zero";
    if (req->hasParam("type")) type = req->getParam("type")->value();

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
    doc["abc"] = mhz.getABC();
    doc["co2"] = latest.co2;
    doc["mhzOk"] = mhzOk;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    std::vector<String> names;
    listFiles(LOGS_DIR, names);
    for (auto &n : names) LittleFS.remove(n);
    req->send(200, "text/plain", "Логи удалены");
  });

  server.begin();
}

// ----------------------------- СЕТЬ -----------------------------
static void setupNetwork() {
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[NET] STA подключено, IP: %s\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    }
  });

  if (cfgSsid.length()) {
    Serial.println("[NET] Пробую подключиться к: " + cfgSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);       // AP остаётся как резерв
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    apMode = (WiFi.status() != WL_CONNECTED);
    if (apMode) Serial.println("[NET] Не удалось, работаю в режиме точки доступа");
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
  }

  if (apMode) {
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("[NET] AP: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

// ----------------------------- SETUP / LOOP -----------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CO2 Monitor ESP32-S2 ===");

  prefs.begin("cfg", false);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS отформатирован");
  }
  if (!LittleFS.exists(LOGS_DIR)) LittleFS.mkdir(LOGS_DIR);

  // DHT22
  dht.setType(22);

  // MH-Z19
  mhzSerial.begin(9600);
  mhz.begin(mhzSerial);
  mhz.setFilter(true, false);            // скользящее среднее
  mhz.setRange(5000);
  mhz.autoCalibration(prefs.getBool("abc", true), 24);
  delay(3000);                           // прогрев

  setupNetwork();
  setupWeb();

  // первая точка сразу, дальше — каждые 10 минут
  readSensors();
  lastLogMs = millis();
  Serial.println("[INIT] готово");
}

void loop() {
  dnsServer.processNextRequest();

  // чтение датчиков раз в 5 секунд
  static uint32_t lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();
    readSensors();
    tryNtp();
  }

  // запись каждые 10 минут
  if (millis() - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = millis();
    latest.timestamp = timeSynced ? (uint32_t)time(nullptr) : (uint32_t)(millis() / 1000);
    if (dhtOk || mhzOk) logReading(latest);
  }

  delay(50);
}

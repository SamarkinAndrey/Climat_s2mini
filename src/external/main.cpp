// ============================================================
// Внешний блок зимней метеостанции: ESP32-C3 SuperMini + BME280
// Питание: 18650 через повышающий преобразователь (или LiPo charger)
// Логика: проснулся -> замерил BME280 -> отправил пакет по ESP-NOW
//        внутреннему блоку -> уснул (deep sleep) на SEND_PERIOD_SEC.
// Пакет принимается firmware.cpp внутреннего блока (onEspNowRecv),
// структура SensorPacket должна совпадать с прошивкой приёмника!
//
// Настройка: укажите MAC внутреннего блока в PEER_MAC ниже
// (печатается в логе внутреннего блока: [ESPNOW] приём включён, MAC...).
//
// Сборка: pio run -e esp32c3_supermini -t upload
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <wifi.h>

#define SEND_PERIOD_SEC 60UL      // период пробуждения и отправки
#define ADC_ATTEN       ADC_11db  // для делителя напряжения аккумулятора
#define BAT_ADC_PIN     2         // GPIO2 — ADC1_CH1 на ESP32-C3

// MAC внутреннего блока (STA-адрес LOLIN S2 Mini). Замените на реальный!
static const uint8_t PEER_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Структура пакета — копия SensorPacket из firmware.cpp (S2).
// Оба чипа little-endian, выравнивание одинаково (4 байта).
struct SensorPacket {
  uint32_t magic;     // 0x434C4931 ("CLI1")
  float    temp;      // температура, °C
  float    hum;       // влажность, %
  float    press;     // давление, гПа
  float    batt;      // напряжение аккумулятора, В
  uint32_t uptime;    // секунды работы с последней перезагрузки
  uint32_t timestamp; // 0 — внутренний блок подставит своё время
};
static const uint32_t EXT_MAGIC = 0x434C4931UL;

Adafruit_BME280 bme;

static float readBattery() {
  // Делитель 2:1 (два резистора 100k) на GPIO2: Vbat = Vadc * 2.
  // Калибровка грубая, для тренда разряда этого достаточно.
  uint32_t raw = analogReadMilliVolts(BAT_ADC_PIN);
  return (raw / 1000.0f) * 2.0f;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  analogSetAttenuation(ADC_ATTEN);

  Wire.begin(/*SDA=*/8, /*SCL=*/9); // кастомные пины C3 SuperMini
  bool bmeOk = bme.begin(0x76, &Wire);
  if (!bmeOk)
    bmeOk = bme.begin(0x77, &Wire);
  if (!bmeOk)
    Serial.println("[BME280] не найден — отправлю нули");

  // ESP-NOW работает без подключения к сети, но Wi-Fi должен быть включён.
  // Канал выбираем тот же, что у внутренней станции (обычно 1–13):
  // при статическом канале можно вызвать WiFi.setChannel().
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ошибка инициализации");
  } else {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER_MAC, 6);
    peer.channel = 0; // текущий канал
    peer.ifidx   = ESP_IF_WIFI_STA;
    if (memcmp(PEER_MAC, "\0\0\0\0\0\0", 6) == 0) {
      Serial.println("[ESPNOW] PEER_MAC не задан — пакет никуда не уйдёт");
    } else if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("[ESPNOW] не удалось добавить получателя");
    }
  }

  SensorPacket pkt = {};
  pkt.magic     = EXT_MAGIC;
  pkt.temp      = bmeOk ? bme.readTemperature() : NAN;
  pkt.hum       = bmeOk ? bme.readHumidity() : NAN;
  pkt.press     = bmeOk ? (bme.readPressure() / 100.0f) : NAN; // гПа
  pkt.batt      = readBattery();
  pkt.uptime    = millis() / 1000UL;
  pkt.timestamp = 0;

  // NaN физически отсеется приёмником; лучше явно пропустить битый замер
  if (isnan(pkt.temp)) {
    Serial.println("[BME280] температура NaN — пропускаю цикл");
  } else {
    esp_err_t err = esp_now_send(PEER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
    Serial.printf("[SEND] t=%.2f h=%.1f p=%.1f batt=%.2fV -> %s\n",
                  pkt.temp, pkt.hum, pkt.press, pkt.batt,
                  err == ESP_OK ? "OK" : esp_err_to_name(err));
  }

  delay(50); // дать Wi-Fi отправить кадр до сна

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_bt_controller_disable(); // C3: BLE выключен по умолчанию, но на всякий случай
  esp_now_deinit();

  Serial.printf("[SLEEP] глубокий сон %lu с\n", SEND_PERIOD_SEC);
  Serial.flush();
  esp_deep_sleep_start();
}

void loop() {
  // недостижимо: после setup() всегда уходит в deep sleep
}

#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <wifi.h>

#define SEND_PERIOD_SEC 60UL
#define ADC_ATTEN       ADC_11db
#define BAT_ADC_PIN     2

static const uint8_t PEER_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

struct SensorPacket {
  uint32_t magic;
  float    temp;
  float    hum;
  float    press;
  float    batt;
  uint32_t uptime;
  uint32_t timestamp;
};

static const uint32_t EXT_MAGIC = 0x434C4931UL;

Adafruit_BME280 bme;

static float readBattery() {
  uint32_t raw = analogReadMilliVolts(BAT_ADC_PIN);
  return (raw / 1000.0f) * 2.0f;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  analogSetAttenuation(ADC_ATTEN);

  Wire.begin(/*SDA=*/8, /*SCL=*/9);
  bool bmeOk = bme.begin(0x76, &Wire);
  if (!bmeOk)
    bmeOk = bme.begin(0x77, &Wire);
  if (!bmeOk)
    Serial.println("[BME280] не найден — отправлю нули");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ошибка инициализации");
  } else {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER_MAC, 6);
    peer.channel = 0;
    peer.ifidx   = ESP_IF_WIFI_STA;
    if (memcmp(PEER_MAC, "\0\0\0\0\0\0", 6) == 0) {
      Serial.println("[ESPNOW] PEER_MAC не задан — пакет никуда не уйдёт");
    } else if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("[ESPNOW] не удалось добавить получателя");
    }
  }

  SensorPacket pkt = {};
  pkt.magic        = EXT_MAGIC;
  pkt.temp         = bmeOk ? bme.readTemperature() : NAN;
  pkt.hum          = bmeOk ? bme.readHumidity() : NAN;
  pkt.press        = bmeOk ? (bme.readPressure() / 100.0f) : NAN;
  pkt.batt         = readBattery();
  pkt.uptime       = millis() / 1000UL;
  pkt.timestamp    = 0;

  if (isnan(pkt.temp)) {
    Serial.println("[BME280] температура NaN — пропускаю цикл");
  } else {
    esp_err_t err = esp_now_send(PEER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
    Serial.printf("[SEND] t=%.2f h=%.1f p=%.1f batt=%.2fV -> %s\n",
                  pkt.temp, pkt.hum, pkt.press, pkt.batt,
                  err == ESP_OK ? "OK" : esp_err_to_name(err));
  }

  delay(50);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_bt_controller_disable();
  esp_now_deinit();

  Serial.printf("[SLEEP] глубокий сон %lu с\n", SEND_PERIOD_SEC);
  Serial.flush();
  esp_deep_sleep_start();
}

void loop() {
}

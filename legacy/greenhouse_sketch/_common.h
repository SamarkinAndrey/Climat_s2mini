/*
  Медианный фильтр — довольно простая и интересная штука. Берёт значения и выбирает из них среднее.
  Не усредняет, а именно ВЫБИРАЕТ, отбрасывает все сильно отличющиеся.
  Время выполнения близко к нулю мкс
  Простой пример, чем отличается медианный фильтр от среднего арифметического:
  Возьмём числа 3, 4, 50. Среднее арифметическое даст нам 19. Целью медианного фильтра является фильтрация
  резких скачков, и после фильтрации он даст нам 4, как среднее между 3 и 50, а 50 будет отброшено как скачок.
  В данном скетче реализована фильтрация по трём значениям. Если интересен вариант с фильтрацией более трёх значений,
  то добро пожаловать в исходную статью. Осторожно, жесть. http://tqfp.org/programming/mediannyy-filtr-na-sluzhbe-razrabotchika.html
*/
/*
  // медианный фильтр из 3ёх значений

  float middle_of_3(float a, float b, float c) {
    int middle;
    if ((a <= b) && (a <= c)) {
      middle = (b <= c) ? b : c;
    }
    else {
      if ((b <= a) && (b <= c)) {
        middle = (a <= c) ? a : c;
      }
      else {
        middle = (a <= b) ? a : b;
      }
    }
    return middle;
  }
*/

/*
  Готовая функция для вычисления среднего арифметического
  Принимает новые значения, суммирует их в своём массиве

  оптимизированный вариант без суммирования массива при каждом вызове
  значения хранятся и отнимаются из переменной суммы
*/
/*
  #define NUM_AVER 10   // выборка (из скольки усредняем)
  int aver(int val) {
    static int t = 0;
    static int vals[NUM_AVER];
    static int average = 0;
    if (++t >= NUM_AVER) t = 0; // перемотка t
    average -= vals[t];     // вычитаем старое
    average += val;         // прибавляем новое
    vals[t] = val;          // запоминаем в массив
    return (average / NUM_AVER);
  }
*/

/*
  Самый продвинутый вариант буфера — кольцевой.
  Данный буфер позволяет хранить набор значений, получать самое крайнее, знать,
  сколько значений осталось непрочитанными, и «добавлять» новые значения в очередь.
  Суть состоит в том, что мы запоминаем ячейки начала и конца последовательности данных,
  и можем обращаться к самому «крайнему» значению, в то же время зная, сколько непрочитанных значений осталось.
  Такой буфер работает быстрее линейного буфера за счёт отсутствия «перемотки» данных на ячейку назад — здесь все данные сидят в своих ячейках,
  меняется только их «адрес» — начало и конец буфера, голова и хвост.
  Такой буфер обычно используется для работы с интерфейсами передачи данных, где всё время что-то читается и добавляется.
  Пример с готовыми функциями по работе с буфером:
*/
/*
  // пример кольцевого буфера для хранения набора данных
  #define buffer_SIZE 32    // размер буфера
  int buffer[buffer_SIZE];  // сам буфер (массив)
  uint8_t buffer_head;      // "голова" буфера
  uint8_t buffer_tail;      // "хвост" буфера
  void setup() {}
  void loop() {}
  // запись в буфер
  void bufferWrite(int newVal) {
    // положение нового значения в буфере
    uint8_t i = (buffer_head + 1 >= buffer_SIZE) ? 0 : buffer_head + 1;

    // если есть местечко
    if (i != buffer_tail) {
      buffer[buffer_head] = newVal; // пишем в буфер
      buffer_head = i;              // двигаем голову
    }
  }
  // чтение из буфера
  int bufferRead() {
    if (buffer_head == buffer_tail) return -1;  // буфер пуст
    int thisVal = buffer[buffer_tail];          // берём с хвоста
    if (++buffer_tail >= buffer_SIZE) buffer_tail = 0;  // хвост двигаем
    return thisVal;   // возвращаем значение
  }
  // возвращает крайнее значение без удаления из буфера
  // если буфер пуст, вернёт -1
  int bufferPeek() {
    return (buffer_head != buffer_tail) ? buffer[buffer_tail] : -1;
  }
  // вернёт количество непрочитанных элементов
  // если буфер пуст, вернёт -1
  int bufferAmount() {
    return ((unsigned int)(buffer_SIZE + buffer_head - buffer_tail)) % buffer_SIZE;
  }
  // "очистка" буфера
  void bufferClear() {
    buffer_head = buffer_tail = 0;
  }
*/

#pragma once

#define DHT_TYPE 22

// #define DHT_PIN    1
// #define DHT_EX_PIN 2

// #define CO2_RX_PIN    36
// #define CO2_TX_PIN    35
// #define CO2_EX_RX_PIN 38
// #define CO2_EX_TX_PIN 37

// #define FAN_MAIN_PIN   3
// #define FAN_INNER_PIN  4
// #define HEATER_PIN     5
// #define HUMIDIFIER_PIN 6

#define DHT_PIN    16
#define DHT_EX_PIN 17

#define CO2_RX_PIN    5
#define CO2_TX_PIN    23
#define CO2_EX_RX_PIN 18
#define CO2_EX_TX_PIN 19

#define FAN_MAIN_PIN   33
#define FAN_INNER_PIN  32
#define HEATER_PIN     25
#define HUMIDIFIER_PIN 26

// #define DHT_PIN    1
// #define DHT_EX_PIN 2

// #define CO2_RX_PIN    18
// #define CO2_TX_PIN    8
// #define CO2_EX_RX_PIN 21
// #define CO2_EX_TX_PIN 5

// #define FAN_MAIN_PIN   6
// #define FAN_INNER_PIN  6
// #define HEATER_PIN     6
// #define HUMIDIFIER_PIN 6

#define CO2_MAX_RANGE      5000
#define MHZ19_HEATING_TIME (1000UL * 60UL * 3UL)

#define WIFI_SSID "Andrey_Lan"
#define WIFI_PASS "2p0r1o8w"

#define WIFI_CHECK_INTERVAL 1000UL * 60UL
#define WEB_UPDATE_INTERVAL 1000UL

#define READINGS_HISTORY_SIZE 900
#define READINGS_STACK_SIZE   1

#define HUMIDIFIER_HARDWARE_DELAY 1000UL

#define SENSOR_SCAN_INTERVAL 1000UL * 2UL
#define SENSOR_SYNC_INTERVAL 1000UL * 60UL * 20UL

#define SENSOR_CHECK_COUNT 3
#define SENSOR_CHECK_DELAY 100UL

// #define PLOT
// #define PLOT_FILE "/plot.tbl"

#define FOR_iu(from, to) for (int i = (from); i < (to); i++)
// FOR_iu(0, 10) {
//   Serial.println(i);
// }

#define FOR_id(from, to) for (int i = (to); i > (from); i--)
// FOR_id(10, 0) {
//   Serial.println(i);
// }

#define FOR_u(x, from, to) for (int(x) = (from); (x) < (to); (x)++)
// FOR_u(i, 0, 10) {
//   FOR_u(j, 0, 3) {
//     someArray[i][j] = someValue;
//   }
// }

#define FOR_d(x, from, to) for (int(x) = (to); (x) > (from); (x)--)
// FOR_d(i, 10, 0) {
//   FOR_d(j, 3, 0) {
//     someArray[i][j] = someValue;
//   }
// }

#define DEBUG_ENABLE

/*
#ifdef DEBUG_ENABLE
 #define DEBUG(_STRING)     \
   {                        \
     Serial.print(_STRING); \
   }
 #define DEBUGln(_STRING)     \
   {                          \
     Serial.println(_STRING); \
   }
 #define DEBUGf(_FORMAT, ...)             \
   {                                      \
     Serial.printf(_FORMAT, __VA_ARGS__); \
   }
 #define tDEBUG(_STRING)  \
   {                      \
     _print_time(Serial); \
     DEBUG(_STRING);      \
   }
 #define tDEBUGln(_STRING) \
   {                       \
     _print_time(Serial);  \
     DEBUGln(_STRING);     \
   }
 #define tDEBUGf(_FORMAT, ...)     \
   {                               \
     _print_time(Serial);          \
     DEBUGf(_FORMAT, __VA_ARGS__); \
   }
 #define LOG(_STRING)       \
   {                        \
     logger.print(_STRING); \
   }
 #define LOGln(_STRING)       \
   {                          \
     logger.println(_STRING); \
   }
 #define LOGf(_FORMAT, ...)               \
   {                                      \
     logger.printf(_FORMAT, __VA_ARGS__); \
   }
 #define tLOG(_STRING)    \
   {                      \
     _print_time(logger); \
     LOG(_STRING);        \
   }
 #define tLOGln(_STRING)  \
   {                      \
     _print_time(logger); \
     LOGln(_STRING);      \
   }
 #define tLOGf(_FORMAT, ...)     \
   {                             \
     _print_time(logger);        \
     LOGf(_FORMAT, __VA_ARGS__); \
   }
#else
 #define DEBUG(_STRING)
 #define DEBUGln(_STRING)
 #define DEBUGf(_FORMAT, ...)
 #define tDEBUG(_STRING)
 #define tDEBUGln(_STRING)
 #define tDEBUGf(_FORMAT, ...)
 #define LOG(_STRING)
 #define LOGln(_STRING)
 #define LOGf(_FORMAT, ...)
 #define tLOG(_STRING)
 #define tLOGln(_STRING)
 #define tLOGf(_FORMAT, ...)
#endif
*/

#include "_debug.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <Arduino.h>
#include <GyverDBFile.h>
#include <LittleFS.h>
#include <SettingsGyverWS.h>
#include <WiFi.h>
#include <WiFiConnector.h>
#include <esp_wifi.h>

using _callback = std::function<void()>;

GyverDBFile     db(&LittleFS, "/green_house.db");
SettingsGyverWS sett("🍄 GreenHouse", &db);

sets::Logger logger(1500);

_Debug debug(Serial, logger);

enum _SensorType {
  dht22in,
  dht22out,
  mhz19in,
  mhz19out
};

enum dbParams : size_t {
  DebugMode,
  SensorScanDelay,

  TemperatureIn,
  HumidityIn,
  CO2In,

  TemperatureOut,
  HumidityOut,
  CO2Out,

  DHT22InExists,
  DHT22OutExists,
  MHZ19InExists,
  MHZ19OutExists,

  MHZ19InHeating,
  MHZ19OutHeating,

  TemperatureControlEnabled,
  TemperatureAlarmThreshold_LOW,
  TemperatureAlarmThreshold_HIGH,
  TemperatureFanDuration,
  TemperatureFanDelay,
  TemperatureHeatingDuration,
  TemperatureHeatingDelay,

  HumidityControlEnabled,
  HumidityAlarmThreshold_LOW,
  HumidityAlarmThreshold_HIGH,
  HumidityFanDuration,
  HumidityFanDelay,
  HumidityWettingDuration,
  HumidityWettingDelay,

  CO2ControlEnabled,
  CO2AlarmThreshold_HIGH,
  CO2FanDuration,
  CO2FanDelay,

  TemperatureInStackSize,
  HumidityInStackSize,
  CO2InStackSize,

  TemperatureOutStackSize,
  HumidityOutStackSize,
  CO2OutStackSize,

  TemperatureOutOffset,
  HumidityOutOffset,
  CO2OutOffset,

#ifdef PLOT
  DisplayModeTabIn,
  DisplayModeTabOut,
#endif

  CO2InRange,
  CO2OutRange,
  CO2InAutoCalibration,
  CO2OutAutoCalibration,

  TemperatureIsSync,
  HumidityIsSync,
  CO2IsSync,

  TemperatureSyncTime,
  HumiditySyncTime,
  CO2SyncTime,

  TemperatureSyncPercent,
  HumiditySyncPercent,
  CO2SyncPercent,

  TemperatureSyncConfirm,
  HumiditySyncConfirm,
  CO2SyncConfirm,

  TemperatureSyncCancelConfirm,
  HumiditySyncCancelConfirm,
  CO2SyncCancelConfirm,

  TemperatureClearOffsetConfirm,
  HumidityClearOffsetConfirm,
  CO2ClearOffsetConfirm,

  CO2InCalibrationConfirm,
  CO2OutCalibrationConfirm,
  RestartConfirm
};

const char *dbParamsName[] = {
    "DebugMode",
    "SensorScanDelay",

    "TemperatureIn",
    "HumidityIn",
    "CO2In",

    "TemperatureOut",
    "HumidityOut",
    "CO2Out",

    "DHT22InExists",
    "DHT22OutExists",
    "MHZ19InExists",
    "MHZ19OutExists",

    "MHZ19InHeating",
    "MHZ19OutHeating",

    "TemperatureControlEnabled",
    "TemperatureAlarmThreshold_LOW",
    "TemperatureAlarmThreshold_HIGH",
    "TemperatureFanDuration",
    "TemperatureFanDelay",
    "TemperatureHeatingDuration",
    "TemperatureHeatingDelay",

    "HumidityControlEnabled",
    "HumidityAlarmThreshold_LOW",
    "HumidityAlarmThreshold_HIGH",
    "HumidityFanDuration",
    "HumidityFanDelay",
    "HumidityWettingDuration",
    "HumidityWettingDelay",

    "CO2ControlEnabled",
    "CO2AlarmThreshold_HIGH",
    "CO2FanDuration",
    "CO2FanDelay",

    "TemperatureInStackSize",
    "HumidityInStackSize",
    "CO2InStackSize",

    "TemperatureOutStackSize",
    "HumidityOutStackSize",
    "CO2OutStackSize",

    "TemperatureOutOffset",
    "HumidityOutOffset",
    "CO2OutOffset",

#ifdef PLOT
    "DisplayModeTabIn",
    "DisplayModeTabOut",
#endif

    "CO2InRange",
    "CO2OutRange",
    "CO2InAutoCalibration",
    "CO2OutAutoCalibration",

    "TemperatureIsSync",
    "HumidityIsSync",
    "CO2IsSync",

    "TemperatureSyncTime",
    "HumiditySyncTime",
    "CO2SyncTime",

    "TemperatureSyncPercent",
    "HumiditySyncPercent",
    "CO2SyncPercent",

    "TemperatureSyncConfirm",
    "HumiditySyncConfirm",
    "CO2SyncConfirm",

    "TemperatureSyncCancelConfirm",
    "HumiditySyncCancelConfirm",
    "CO2SyncCancelConfirm",

    "TemperatureClearOffsetConfirm",
    "HumidityClearOffsetConfirm",
    "CO2ClearOffsetConfirm",

    "CO2InCalibrationConfirm",
    "CO2OutCalibrationConfirm",
    "RestartConfirm"};

class _UsingName {
protected:
  char _Name[50] = "";

public:
  virtual void setName(const char Name[] = nullptr, const char Prefix[] = nullptr) {
    _Name[0] = '\0';

    if (Prefix && (strlen(Prefix) > 0))
      strcat(_Name, Prefix);

    if (Name && (strlen(Name) > 0)) {
      if (strlen(_Name) > 0)
        strcat(_Name, ".");

      strcat(_Name, Name);
    }
  }

  virtual const char *getName() {
    return _Name;
  }
};

class _UsingIniciator {
protected:
  void *_Iniciator = nullptr;

private:
  virtual void setIniciator(void *Iniciator = nullptr) {
    _Iniciator = Iniciator;
  }

  virtual const void *getIniciator() {
    return _Iniciator;
  }
};

void MillisToTimeStr(char _buffer[], ulong _millis) {
  uint32_t sec = _millis / 1000UL;

  int hours = (sec / 3600UL);
  int mins  = (sec % 3600UL) / 60UL;
  int secs  = (sec % 3600UL) % 60UL;

  sprintf(_buffer, "%02d:%02d:%02d", hours, mins, secs);
}

void MillisToTimeStr24(char _buffer[], ulong _millis) {
  uint32_t sec = _millis / 1000UL;

  int hours = (sec / 3600UL) % 24;
  int mins  = (sec % 3600UL) / 60UL;
  int secs  = (sec % 3600UL) % 60UL;

  sprintf(_buffer, "%02d:%02d:%02d", hours, mins, secs);
}

float ifnan(float Value) {
  return isnan(Value) ? 0 : Value;
}

double ifnan(double Value) {
  return isnan(Value) ? 0 : Value;
}

unsigned long ifnan(unsigned long Value) {
  return isnan(Value) ? 0 : Value;
}

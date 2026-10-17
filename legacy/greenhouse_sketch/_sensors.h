#pragma once

#include <DHT.h>
#include <MHZ19.h>
#include <SoftwareSerial.h>
#include <dhtnew.h>

#include "_readings.h"
#include "_sensors_sync.h"

class _SensorCustom : public _UsingName {
private:
  bool _SetActive(bool Value) {
    if (Value) {
      if (!_IsActive) {
        _StartMillis = millis();
        _IsActive    = true;
      }
    } else {
      if (_IsActive) {
        Reload();

        _StartMillis = 0;
        _IsActive    = false;
      }
    }
    return _IsActive;
  }

public:
  _SensorCustom(ulong HeatingTime = 0) {
    setHeatingTime(HeatingTime);
  }

  void Init() {
    OnInit();
  }

  void Update() {
    bool _result = GetReadings();
    for (int i = 1; ((i < SENSOR_CHECK_COUNT) && !_result); i++) {
      delay(SENSOR_CHECK_DELAY);

      _result = GetReadings();
    }

    if (!_SetActive(_result))
      return;

    OnUpdate();
  }

  void Reload() {
    OnReload();
  }

  bool IsActive() {
    return _IsActive;
  }

  bool IsHeating() {
    return (_IsActive && (_HeatingTime > 0) && ((millis() - _StartMillis) < _HeatingTime));
  }

  float HeatingPercent() {
    if (_IsActive) {
      if (_HeatingTime > 0)
        return ((millis() - _StartMillis) * (float)100) / _HeatingTime;
      else
        return (100);
    } else {
      return (0);
    }
  }

  void setHeatingTime(ulong Value = 0) {
    if (Value > 0)
      _HeatingTime = Value;
  }

  ulong getHeatingTime() {
    return _HeatingTime;
  }

  ulong getStartMillis() {
    return _StartMillis;
  }

protected:
  ulong _HeatingTime = 0;
  ulong _StartMillis = 0;

  bool _IsActive = false;

  virtual void OnInit() {
  }

  virtual void OnUpdate() {
  }

  virtual void OnReload() {
  }

  virtual bool GetReadings() {
    return false;
  }
};

class _SensorDHT : public _SensorCustom {
protected:
  float _temperature = 0;
  float _humidity    = 0;

public:
  _Readings Temperature;
  _Readings Humidity;

  _SensorDHT(int    TemperatureHistorySec = 0,
             int    HumidityHistorySec    = 0,
             size_t TemperatureStackSize  = 0,
             size_t HumidityStackSize     = 0)
      : Temperature(TemperatureHistorySec,
                    TemperatureStackSize),
        Humidity(HumidityHistorySec,
                 HumidityStackSize) {
    setName("_SensorDHT");
  }

  virtual void setName(const char Name[] = nullptr, const char Prefix[] = nullptr) override {
    _SensorCustom::setName(Name, Prefix);

    Temperature.History.setName("Temperature.History", _Name);
    Temperature.Stack.setName("Temperature.Stack", _Name);
    Humidity.History.setName("Humidity.History", _Name);
    Humidity.Stack.setName("Humidity.Stack", _Name);
  }

  virtual void OnUpdate() override {
    Temperature.putValue(_temperature);
    Humidity.putValue(_humidity);

    sprintf(Temperature.Text, "%.1f °C", Temperature.Value);
    sprintf(Humidity.Text, "%.1f %%", Humidity.Value);
  }

  virtual void OnReload() override {
    Temperature.Clear();
    Humidity.Clear();
  }

  void setTemperatureOffset(float Value) {
    Temperature.setOffset(Value);

    debug.tprintf("%s.setTemperatureOffset(%.4f)\n", _Name, Value);
  }

  float getTemperatureOffset() {
    return Temperature.getOffset();
  }

  void setHumidityOffset(float Value) {
    Humidity.setOffset(Value);

    debug.tprintf("%s.setHumidityOffset(%.4f)\n", _Name, Value);
  }

  float getHumidityOffset() {
    return Humidity.getOffset();
  }

  void setTemperatureStackSize(size_t Value) {
    Temperature.setStackSize(Value);

    debug.tprintf("%s.setTemperatureStackSize(%d)\n", _Name, Value);
  }

  size_t getTemperatureStackSize() {
    return Temperature.getStackSize();
  }

  void setHumidityStackSize(size_t Value) {
    Humidity.setStackSize(Value);

    debug.tprintf("%s.setHumidityStackSize(%d)\n", _Name, Value);
  }

  size_t getHumidityStackSize() {
    return Humidity.getStackSize();
  }
};

class _SensorDHT22 : public _SensorDHT {
private:
  DHTNEW _dht22;

public:
  _SensorDHT22(int    Pin,
               int    TemperatureHistorySec = 0,
               int    HumidityHistorySec    = 0,
               size_t TemperatureStackSize  = 0,
               size_t HumidityStackSize     = 0)
      : _SensorDHT(TemperatureHistorySec,
                   HumidityHistorySec,
                   TemperatureStackSize,
                   HumidityStackSize),
        _dht22(Pin) {
    setName("_SensorDHT22");

    _dht22.setType(DHT_TYPE);
  }

protected:
  virtual bool GetReadings() override {
    _temperature = _dht22.getTemperature();
    _humidity    = _dht22.getHumidity();

    return (!isnan(_temperature) && !isnan(_humidity));
  }
};

class _SensorAM2320 : public _SensorDHT {
private:
  DHT _am2320;

public:
  _SensorAM2320(int    Pin,
                int    TemperatureHistorySec = 0,
                int    HumidityHistorySec    = 0,
                size_t TemperatureStackSize  = 0,
                size_t HumidityStackSize     = 0)
      : _SensorDHT(TemperatureHistorySec,
                   HumidityHistorySec,
                   TemperatureStackSize,
                   HumidityStackSize),
        _am2320(Pin, DHT_TYPE) {
    setName("_SensorAM2320");

    _am2320.begin();
  }

protected:
  virtual bool GetReadings() override {
    _temperature = _am2320.readTemperature();
    _humidity    = _am2320.readHumidity();

    return (!isnan(_temperature) && !isnan(_humidity));
  }
};

class _SensorMHZ19 : public _SensorCustom {
private:
  MHZ19          _mhz19;
  SoftwareSerial _Serial;

public:
  _Readings CO2;

  bool AutoCalibration = false;
  int  Range           = 0;

  _SensorMHZ19(int    rxPin,
               int    txPin,
               ulong  HeatingTime       = 0,
               int    HistoryHistorySec = 0,
               size_t StackSize         = 0)
      : _SensorCustom(HeatingTime),
        CO2(HistoryHistorySec,
            StackSize),
        _Serial(rxPin, txPin) {
    setName("_SensorMHZ19");

    _Serial.begin(9600);
    _mhz19.begin(_Serial);

    AutoCalibration = _mhz19.getABC();
    Range           = _mhz19.getRange();
  }

  virtual void setName(const char Name[] = nullptr, const char Prefix[] = nullptr) override {
    _SensorCustom::setName(Name, Prefix);

    CO2.History.setName("CO2.History", _Name);
    CO2.Stack.setName("CO2.Stack", _Name);
  }

  void setAutoCalibration(bool Value) {
    if (!_IsActive)
      return;

    _mhz19.autoCalibration(Value);
    delay(50);

    AutoCalibration = _mhz19.getABC();

    debug.tprintf("%s.setAutoCalibration(%s)\n", _Name, Value ? "true" : "false");
  }

  void setRange(int Value) {
    if (!_IsActive || (Range = Value) || ((Value != 2000) && (Value != 5000)))
      return;

    _mhz19.setRange(Value);
    delay(50);

    Range = _mhz19.getRange();

    debug.tprintf("%s.setRange(%d)\n", _Name, Range);
  }

  void Calibrate400() {
    if (!_IsActive)
      return;

    if (AutoCalibration) {
      setAutoCalibration(false);

      if (!AutoCalibration)
        _mhz19.calibrate();

      setAutoCalibration(true);
    } else {
      _mhz19.calibrate();
    }

    debug.tprintf("%s.Calibrate400()\n", _Name);
  }

  void setCO2Offset(float Value) {
    CO2.setOffset(Value);

    debug.tprintf("%s.setCO2Offset(%.4f)\n", _Name, Value);
  }

  float getCO2Offset() {
    return CO2.getOffset();
  }

  void setCO2StackSize(size_t Value) {
    CO2.setStackSize(Value);

    debug.tprintf("%s.setCO2StackSize(%d)\n", _Name, Value);
  }

  size_t getCO2StackSize() {
    return CO2.getStackSize();
  }

protected:
  float _co2 = 0;

  virtual bool GetReadings() override {
    _co2 = (float)_mhz19.getCO2();

    return (_co2 > 0);
  }

  void OnUpdate() override {
    if (IsHeating()) {
      sprintf(CO2.Text, "♨ %02d %%", (int)round(HeatingPercent()));

      return;
    }

    CO2.putValue(_co2);
    sprintf(CO2.Text, "%d ppm", (int)round(CO2.Value));
  }

  void OnReload() override {
    CO2.Clear();
  }
};

class _SensorList {
private:
  float _TemperatureOffsetSave = 0;
  float _HumidityOffsetSave    = 0;
  float _CO2OffsetSave         = 0;

  _callback _temperature_before_sync = nullptr;
  _callback _humidity_before_sync    = nullptr;
  _callback _co2_before_sync         = nullptr;

  _callback _temperature_after_sync = nullptr;
  _callback _humidity_after_sync    = nullptr;
  _callback _co2_after_sync         = nullptr;

  void _TemperatureBeforeSync() {
    _TemperatureOffsetSave = dht22out.getTemperatureOffset();

    setTemperatureHistoryEnabled(&TemperatureSync, true);

    dht22out.setTemperatureOffset(0);
    dht22out.Temperature.Stack.clear();

    if (_temperature_before_sync)
      _temperature_before_sync();
  }

  void _HumidityBeforeSync() {
    _HumidityOffsetSave = dht22out.getHumidityOffset();

    setHumidityHistoryEnabled(&HumiditySync, true);

    dht22out.setHumidityOffset(0);
    dht22out.Humidity.Stack.clear();

    if (_humidity_before_sync)
      _humidity_before_sync();
  }

  void _CO2BeforeSync() {
    _CO2OffsetSave = mhz19out.getCO2Offset();

    setCO2HistoryEnabled(&CO2Sync, true);

    mhz19out.setCO2Offset(0);
    mhz19out.CO2.Stack.clear();

    if (_co2_before_sync)
      _co2_before_sync();
  }

  void _TemperatureAfterSync() {
    _IntervalData _in  = dht22in.Temperature.History.getIntervalStat(SENSOR_SYNC_INTERVAL);
    _IntervalData _out = dht22out.Temperature.History.getIntervalStat(SENSOR_SYNC_INTERVAL);

    dht22out.setTemperatureOffset(_in.ValueAvg - _out.ValueAvg);

    setTemperatureHistoryEnabled(&TemperatureSync, false);

    if (_temperature_after_sync)
      _temperature_after_sync();
  }

  void _HumidityAfterSync() {
    _IntervalData _in  = dht22in.Humidity.History.getIntervalStat(SENSOR_SYNC_INTERVAL);
    _IntervalData _out = dht22out.Humidity.History.getIntervalStat(SENSOR_SYNC_INTERVAL);

    dht22out.setHumidityOffset(_in.ValueAvg - _out.ValueAvg);

    setHumidityHistoryEnabled(&HumiditySync, false);

    if (_humidity_after_sync)
      _humidity_after_sync();
  }

  void _CO2AfterSync() {
    _IntervalData _in  = mhz19in.CO2.History.getIntervalStat(SENSOR_SYNC_INTERVAL);
    _IntervalData _out = mhz19out.CO2.History.getIntervalStat(SENSOR_SYNC_INTERVAL);

    mhz19out.setCO2Offset(_in.ValueAvg - _out.ValueAvg);

    setCO2HistoryEnabled(&CO2Sync, false);

    if (_co2_after_sync)
      _co2_after_sync();
  }

  void _TemperatureCancelSync() {
    dht22out.setTemperatureOffset(_TemperatureOffsetSave);

    setTemperatureHistoryEnabled(&TemperatureSync, false);

    if (_temperature_after_sync)
      _temperature_after_sync();
  }

  void _HumidityCancelSync() {
    dht22out.setHumidityOffset(_HumidityOffsetSave);

    setHumidityHistoryEnabled(&HumiditySync, false);

    if (_humidity_after_sync)
      _humidity_after_sync();
  }

  void _CO2CancelSync() {
    mhz19out.setCO2Offset(_CO2OffsetSave);

    setCO2HistoryEnabled(&CO2Sync, false);

    if (_co2_after_sync)
      _co2_after_sync();
  }

public:
  _SensorAM2320 dht22in;
  _SensorAM2320 dht22out;
  _SensorMHZ19  mhz19in;
  _SensorMHZ19  mhz19out;

  _SensorSync TemperatureSync;
  _SensorSync HumiditySync;
  _SensorSync CO2Sync;

  _SensorList() : dht22in(DHT_PIN),
                  dht22out(DHT_EX_PIN),
                  mhz19in(CO2_RX_PIN, CO2_TX_PIN, MHZ19_HEATING_TIME),
                  mhz19out(CO2_EX_RX_PIN, CO2_EX_TX_PIN, MHZ19_HEATING_TIME) {
    dht22in.setName("AM2320(inner)");
    dht22out.setName("AM2320(outer)");
    mhz19in.setName("MH-Z19B(inner)");
    mhz19out.setName("MH-Z19B(outer)");

    TemperatureSync.setName("TemperatureSync");
    HumiditySync.setName("HumiditySync");
    CO2Sync.setName("CO2Sync");

    TemperatureSync.OnBeforeSync([this]() { this->_TemperatureBeforeSync(); });
    HumiditySync.OnBeforeSync([this]() { this->_HumidityBeforeSync(); });
    CO2Sync.OnBeforeSync([this]() { this->_CO2BeforeSync(); });

    TemperatureSync.OnAfterSync([this]() { this->_TemperatureAfterSync(); });
    HumiditySync.OnAfterSync([this]() { this->_HumidityAfterSync(); });
    CO2Sync.OnAfterSync([this]() { this->_CO2AfterSync(); });

    TemperatureSync.OnCancelSync([this]() { this->_TemperatureCancelSync(); });
    HumiditySync.OnCancelSync([this]() { this->_HumidityCancelSync(); });
    CO2Sync.OnCancelSync([this]() { this->_CO2CancelSync(); });
  }

  // void Init() {
  //   dht22in.Init();
  //   dht22out.Init();
  //   mhz19in.Init();
  //   mhz19out.Init();
  // }

  void setTemperatureHistoryEnabled(void *Iniciator, bool Value) {
    dht22in.Temperature.History.setEnabled(Iniciator, Value);
    dht22out.Temperature.History.setEnabled(Iniciator, Value);
  }

  void setHumidityHistoryEnabled(void *Iniciator, bool Value) {
    dht22in.Humidity.History.setEnabled(Iniciator, Value);
    dht22out.Humidity.History.setEnabled(Iniciator, Value);
  }

  void setCO2HistoryEnabled(void *Iniciator, bool Value) {
    mhz19in.CO2.History.setEnabled(Iniciator, Value);
    mhz19out.CO2.History.setEnabled(Iniciator, Value);
  }

  void Update() {
    dht22in.Update();
    dht22out.Update();
    mhz19in.Update();
    mhz19out.Update();

    TemperatureSync.Tick();
    HumiditySync.Tick();
    CO2Sync.Tick();
  }

  void OnTemperatureBeforeSync(_callback cb) {
    if (cb)
      _temperature_before_sync = cb;
  }

  void OnHumidityBeforeSync(_callback cb) {
    if (cb)
      _humidity_before_sync = cb;
  }

  void OnCO2BeforeSync(_callback cb) {
    if (cb)
      _co2_before_sync = cb;
  }

  void OnTemperatureAfterSync(_callback cb) {
    if (cb)
      _temperature_after_sync = cb;
  }

  void OnHumidityAfterSync(_callback cb) {
    if (cb)
      _humidity_after_sync = cb;
  }

  void OnCO2AfterSync(_callback cb) {
    if (cb)
      _co2_after_sync = cb;
  }
};

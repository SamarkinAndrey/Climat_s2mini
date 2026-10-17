#pragma once

#include "_common.h"
#include "_devices.h"
#include "_sensors.h"

using VL = float[];

class _SensorList;
class _DeviceList;
class _AlertList;

class _GreenHouse {
private:
  _callback _on_web_update     = nullptr;
  _callback _on_alerts_update  = nullptr;
  _callback _on_devices_update = nullptr;

  GyverDBFile     *_db   = nullptr;
  SettingsGyverWS *_sett = nullptr;

  ulong _SensorUpdateInterval = SENSOR_SCAN_INTERVAL;
  ulong _WebUpdateInterval    = WEB_UPDATE_INTERVAL;

  ulong _SensorTickMillis = 0;
  ulong _WebTickMillis    = 0;

  bool _SensorsTick() {
    if ((_SensorTickMillis == 0) || ((millis() - _SensorTickMillis) > getSensorUpdateInterval())) {
      _SensorTickMillis = millis();

      SensorsUpdate();

      return true;
    }
    return false;
  }

  bool _WebTick() {
    if ((_SensorTickMillis > 0) && ((millis() - _WebTickMillis) > getWebUpdateInterval())) {
      _WebTickMillis = millis();

      WebUpdate();

      return true;
    }
    return false;
  }

public:
  _SensorList Sensors;
  _DeviceList Devices;
  _AlertList  Alerts;

  _GreenHouse(GyverDBFile *db = nullptr, SettingsGyverWS *sett = nullptr) {
    _db   = db;
    _sett = sett;
  }

  ~_GreenHouse() {
    if (_db)
      _db = nullptr;
    if (_sett)
      _sett = nullptr;
  }

  void setSensorUpdateInterval(ulong Value = 0) {
    if (Value > 1999)
      _SensorUpdateInterval = Value;
  }

  ulong getSensorUpdateInterval() {
    return _SensorUpdateInterval;
  }

  void setWebUpdateInterval(ulong Value = 0) {
    if (Value > 999)
      _WebUpdateInterval = Value;
  }

  ulong getWebUpdateInterval() {
    return _WebUpdateInterval;
  }

  void SensorsUpdate() {
    Sensors.Update();

    AlertsUpdate();
  }

  void AlertsUpdate() {
    if (_on_alerts_update) {
      _on_alerts_update();

      DevicesUpdate();
    }
  }

  void DevicesUpdate() {
    if (_on_devices_update) {
      _on_devices_update();

      WebUpdate();
    }
  }

  void WebUpdate() {
    if (_on_web_update) {
      _on_web_update();

      _WebTickMillis = millis();
    }
  }

  void Tick() {
    _SensorsTick();
    _WebTick();

    Devices.Tick();
  }

  void OnAlertsUpdate(_callback cb) {
    _on_alerts_update = cb;
  }

  void OnDevicesUpdate(_callback cb) {
    _on_devices_update = cb;
  }

  void OnWebUpdate(_callback cb) {
    _on_web_update = cb;
  }
};

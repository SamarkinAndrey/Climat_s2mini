#pragma once

#include <GyverTimer.h>

class _SensorSync : public _UsingName {
  friend class _SensorList;

private:
  GTimer _Timer;

  ulong _StartMillis  = 0;
  ulong _SyncInterval = 0;

  _callback _on_before_sync = nullptr;
  _callback _on_after_sync  = nullptr;
  _callback _on_cancel_sync = nullptr;

  char _StartMillisStr[50] = "";
  char _PercentStr[50]     = "";

public:
  _SensorSync() {
    _Timer.setMode(MS);
  }

  ulong getSyncInterval() {
    return _SyncInterval;
  }

  ulong getStartMillis() {
    return _StartMillis;
  }

  ulong getMillisLeft() {
    return (_StartMillis > 0) ? (_SyncInterval - (millis() - _StartMillis)) : 0;
  }

  char *getMillisLeftStr() {
    MillisToTimeStr(_StartMillisStr, getMillisLeft());

    return _StartMillisStr;
  }

  float getPercent() {
    return (_StartMillis > 0) ? ((millis() - _StartMillis) * (float)100) / _SyncInterval : 0;
  }

  char *getPercentStr() {
    sprintf(_PercentStr, "%02d %%", (int)round(getPercent()));

    return _PercentStr;
  }

  void Start(ulong SyncInterval = SENSOR_SYNC_INTERVAL) {
    if (SyncInterval < 1)
      return;

    _StartMillis  = millis();
    _SyncInterval = SyncInterval;

    debug.tprintf("%s.Start(%lu)\n", _Name, _SyncInterval);

    if (_on_before_sync)
      _on_before_sync();

    _Timer.setTimeout(_SyncInterval);
  }

  void Cancel() {
    if (!IsActive())
      return;

    if (_Timer.isEnabled())
      _Timer.stop();

    _StartMillis  = 0;
    _SyncInterval = 0;

    if (_on_cancel_sync)
      _on_cancel_sync();
  }

  void Tick() {
    if (!_Timer.isReady())
      return;

    _StartMillis  = 0;
    _SyncInterval = 0;

    if (_on_after_sync)
      _on_after_sync();
  }

  bool IsActive() {
    return (_StartMillis > 0);
  }

  void OnBeforeSync(_callback cb) {
    if (cb)
      _on_before_sync = cb;
  }

  void OnAfterSync(_callback cb) {
    if (cb)
      _on_after_sync = cb;
  }

  void OnCancelSync(_callback cb) {
    if (cb)
      _on_cancel_sync = cb;
  }
};

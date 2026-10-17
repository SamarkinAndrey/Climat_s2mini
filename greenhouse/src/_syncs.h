#pragma once

#include "_classtype.h"
#include <GyverTimer.h>

class _Sync : public _ClassType {
public:
  using _SyncCallback = std::function<void(_Sync &Sync)>;

private:
  GTimer _Timer;

  _Readings *_Readings1 = nullptr;
  _Readings *_Readings2 = nullptr;

  ulong _StartMillis  = 0;
  ulong _SyncInterval = 0;

  float _OffsetSave = 0;

  _SyncCallback _on_before_sync = nullptr;
  _SyncCallback _on_after_sync  = nullptr;
  _SyncCallback _on_cancel_sync = nullptr;

  char _StartMillisStr[50] = "";
  char _PercentStr[50]     = "";

  void _Clear () {
    if (_Timer.isEnabled())
      _Timer.stop();

    _Readings1    = nullptr;
    _Readings2    = nullptr;
    _StartMillis  = 0;
    _SyncInterval = 0;
    _OffsetSave = 0;

    setSubType(_SubType::Undefined);
  }

  bool _BeforeSync() {
    ulong _millis = millis();

    if (_Readings1->StatEnabled() && _Readings2->StatEnabled() &&
        ((_millis - _Readings1->Stat.MillisFrom) > SENSOR_SYNC_INTERVAL) &&
        ((_millis - _Readings2->Stat.MillisFrom) > SENSOR_SYNC_INTERVAL)) {
      _AfterSync();

      return false;
    }

    _OffsetSave = _Readings2->Offset();

    bool _stat1 = _Readings1->setStatEnabled(this, true);
    bool _stat2 = _Readings2->setStatEnabled(this, true);

    if (!_stat1 || !_stat2) {
      _Readings1->setStatEnabled(this, false);
      _Readings2->setStatEnabled(this, false);

      return false;
    }

    _Readings2->setOffset(0);
    _Readings2->Stack.clear();

    if (_on_before_sync)
      _on_before_sync(*this);

    return true;
  }

  void _AfterSync() {
    ulong _millis = millis();

    _Readings2->setOffset(_Readings1->Stat.Avg - _Readings2->Stat.Avg);

    _Readings1->setStatEnabled(this, false);
    _Readings2->setStatEnabled(this, false);

    if (_on_after_sync)
      _on_after_sync(*this);
  }

  void _CancelSync() {
    _Readings2->setOffset(_OffsetSave);

    _Readings1->setStatEnabled(this, false);
    _Readings2->setStatEnabled(this, false);

    if (_on_cancel_sync)
      _on_cancel_sync(*this);
  }

public:
  _Sync() {
    setMainType(_MainType::Sync);

    _Timer.setMode(MS);
  }

  ulong getSyncInterval() {
    return _SyncInterval;
  }

  ulong getStartMillis() {
    return _StartMillis;
  }

  ulong MillisLeft() {
    if (_StartMillis < 1)
      return 0;

    ulong _elapsed = millis() - _StartMillis;

    return (_elapsed < _SyncInterval) ? (_SyncInterval - _elapsed) : 0;
  }

  char *MillisLeftStr() {
    MillisToTimeStr(_StartMillisStr, sizeof(_StartMillisStr), MillisLeft());

    return _StartMillisStr;
  }

  float Percent() {
    if ((_StartMillis < 1) || (_SyncInterval < 1))
      return 0;

    float _percent = ((millis() - _StartMillis) * (float)100) / _SyncInterval;

    return (_percent > 100) ? 100 : _percent;
  }

  char *PercentStr() {
    sprintf(_PercentStr, "%02d %%", (int)round(Percent()));

    return _PercentStr;
  }

  _Readings &Readings1() {
    return *_Readings1;
  }

  const _Readings &Readings1() const {
    return *_Readings1;
  }

  _Readings &Readings2() {
    return *_Readings2;
  }

  const _Readings &Readings2() const {
    return *_Readings2;
  }

  void Start(_Readings *Readings1, _Readings *Readings2, ulong SyncInterval = SENSOR_SYNC_INTERVAL) {
    if (!Readings1 || !Readings2 || (Readings1->Type() != Readings2->Type()) || (SyncInterval < 1))
      return;

    if (IsActive())
      Cancel();

    _Readings1    = Readings1;
    _Readings2    = Readings2;
    _StartMillis  = millis();
    _SyncInterval = SyncInterval;

    setSubType(_Readings2->SubType());

    debug.tprintf("%s.Start(%lu)\n", Name(), _SyncInterval);

    if (!_BeforeSync()) {
      _Clear();
      return;
    }

    _Timer.setTimeout(_SyncInterval);
  }

  void Cancel() {
    if (!IsActive())
      return;

    if (_Timer.isEnabled())
      _Timer.stop();

    _CancelSync();
    _Clear();
  }

  void Tick() {
    if (!_Timer.isReady())
      return;

    _AfterSync();
    _Clear();
  }

  bool IsActive() {
    return (_StartMillis > 0);
  }

  void OnBeforeSync(_SyncCallback cb) {
    if (cb)
      _on_before_sync = cb;
  }

  void OnAfterSync(_SyncCallback cb) {
    if (cb)
      _on_after_sync = cb;
  }

  void OnCancelSync(_SyncCallback cb) {
    if (cb)
      _on_cancel_sync = cb;
  }
};

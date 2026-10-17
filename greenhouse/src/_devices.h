#pragma once

#include "_classtype.h"
#include "_common.h"
#include <GyverTimer.h>

class _DeviceList;

class _Device : public _UsingIniciator, public _ClassType, public _ClassOwner<_DeviceList> {
public:
  using _OnActiveChanged = std::function<void(_Device &Device, bool IsActive)>;
  using _OnStateChanged  = std::function<void(_Device &Device, bool State)>;

private:
  GTimer _DurationTimer;
  GTimer _DelayTimer;

  int _Pin = 0;

  ulong _Duration      = 0;
  ulong _Delay         = 0;
  ulong _TotalDuration = 0;

  int _LoopCount = 0;
  int _LoopNum   = 0;

  ulong _HardwareDelay = 0;

  ulong _ActiveChangeMillis = 0;
  ulong _StateChangeMillis  = 0;

  uint8_t _Level = HIGH;

  bool _IsActive = false;
  bool _State    = false;

  _OnActiveChanged _on_active_changed = nullptr;
  _OnStateChanged  _on_state_changed  = nullptr;

  bool _PinState() {
    return digitalRead(_Pin);
  }

  void _TurnOn() {
    if (_PinState() == _Level)
      return;

    digitalWrite(_Pin, _Level);
    _delay(50);

    _StateChangeMillis = millis();
    _State             = true;

    _StateChanged();

    debug.tprintf("%s.TurnOn()\n", Name());
  }

  void _TurnOff() {
    if (_PinState() != _Level)
      return;

    digitalWrite(_Pin, !_Level);
    _delay(50);

    _StateChangeMillis = millis();
    _State             = false;

    _StateChanged();

    debug.tprintf("%s.TurnOff()\n", Name());
  }

  void _Stop() {
    Stop(_Iniciator);
  }

  void _ActiveChanged() {
    if (_on_active_changed)
      _on_active_changed(*this, _IsActive);
  }

  void _StateChanged() {
    if (_on_state_changed)
      _on_state_changed(*this, _State);
  }

public:
  _Device(_DeviceList &Devices, int Pin, uint8_t Level = HIGH, ulong HardwareDelay = 0)
      : _ClassOwner<_DeviceList>(Devices) {
    _Pin           = Pin;
    _Level         = Level;
    _HardwareDelay = HardwareDelay;

    digitalWrite(_Pin, !_Level);

    pinMode(_Pin, OUTPUT);

    _DurationTimer.setMode(MS);
    _DelayTimer.setMode(MS);

    _TurnOff();
  }

  void Start(void *Iniciator /* = nullptr*/, ulong Duration = 0, ulong Delay = 0, int LoopCount = 0, ulong TotalDuration = 0) {
    if (_IsActive && (_Iniciator != Iniciator))
      return;

    ulong _oldDuration = _Duration;

    _Duration      = (Duration > 0) ? Duration + _HardwareDelay : 0;
    _Delay         = Delay;
    _LoopCount     = LoopCount;
    _TotalDuration = TotalDuration;
    _LoopNum       = 0;

    if (_IsActive) {
      if ((_oldDuration < 1) && (_Duration > 0))
        _DurationTimer.setTimeout(_Duration);

      return;
    }

    debug.tprintf("%s.Start()\n", Name());

    _Iniciator = Iniciator;
    _TurnOn();
    _ActiveChangeMillis = millis();
    _IsActive           = true;

    _ActiveChanged();

    if (_Duration > 0)
      _DurationTimer.setTimeout(_Duration);
  }

  void Stop(void *Iniciator /* = nullptr*/) {
    if (!_IsActive || (_Iniciator != Iniciator))
      return;

    if (_DurationTimer.isEnabled())
      _DurationTimer.stop();

    if (_DelayTimer.isEnabled())
      _DelayTimer.stop();

    debug.tprintf("%s.Stop()\n", Name());

    _Iniciator = Iniciator;
    _TurnOff();
    _ActiveChangeMillis = millis();
    _IsActive           = false;

    _ActiveChanged();
  }

  void setDuration(ulong Value = 0) {
    if (_IsActive)
      _Duration = Value;
  }

  void setDelay(ulong Value = 0) {
    if (_IsActive)
      _Delay = Value;
  }

  void setTotalDuration(ulong Value = 0) {
    if (_IsActive)
      _TotalDuration = Value;
  }

  void setLoopCount(ulong Value = 0) {
    if (_IsActive)
      _LoopCount = Value;
  }

  ulong Duration() {
    return (_Duration > _HardwareDelay) ? _Duration - _HardwareDelay : _Duration;
  }

  ulong Delay() {
    return _Delay;
  }

  ulong TotalDuration() {
    return _TotalDuration;
  }

  ulong LoopCount() {
    return _LoopCount;
  }

  uint8_t Level() {
    return _Level;
  }

  bool State() {
    return _State;
  }

  ulong MillisChanged() {
    return _ActiveChangeMillis;
  }

  ulong MillisPassed() {
    return (_ActiveChangeMillis > 0) ? millis() - _ActiveChangeMillis : 0;
  }

  bool IsWorking() {
    return _IsActive;
  }

  void Tick() {
    if (_IsActive && (_TotalDuration > 0) && (MillisPassed() > _TotalDuration)) {
      _Stop();
      return;
    }

    if (_DurationTimer.isReady()) {
      _TurnOff();
      if (_Delay > 0) {
        _DelayTimer.setTimeout(_Delay);
      } else {
        _Stop();
        return;
      }
    }

    if (_DelayTimer.isReady()) {
      if (_LoopCount > 0) {
        _LoopNum++;
        if (_LoopNum >= _LoopCount) {
          _Stop();
          return;
        }
      }

      if (_Duration > 0) {
        _TurnOn();
        _DurationTimer.setTimeout(_Duration);
      } else {
        _Stop();
        return;
      }
    }
  }

  void OnActiveChanged(_OnActiveChanged cb) {
    if (cb)
      _on_active_changed = cb;
  }

  void OnStateChanged(_OnStateChanged cb) {
    if (cb)
      _on_state_changed = cb;
  }
};

class _DeviceList {
public:
  _Device FanMain;
  _Device FanInner;
  _Device Heater;
  _Device Humidifier;

  _DeviceList() : FanMain(*this, FAN_MAIN_PIN, LOW),
                  FanInner(*this, FAN_INNER_PIN, LOW),
                  Heater(*this, HEATER_PIN, LOW),
                  Humidifier(*this, HUMIDIFIER_PIN, LOW, HUMIDIFIER_HARDWARE_DELAY) {
    FanMain.setType(_MainType::Device, _SubType::FanMain);
    FanInner.setType(_MainType::Device, _SubType::FanInner);
    Heater.setType(_MainType::Device, _SubType::Heater);
    Humidifier.setType(_MainType::Device, _SubType::Humidifier);
  }

  void Tick() {
    FanMain.Tick();
    FanInner.Tick();
    Heater.Tick();
    Humidifier.Tick();
  }

  void OnDeviceActiveChanged(_Device::_OnActiveChanged cb) {
    if (!cb)
      return;

    FanMain.OnActiveChanged(cb);
    FanInner.OnActiveChanged(cb);
    Heater.OnActiveChanged(cb);
    Humidifier.OnActiveChanged(cb);
  }

  void OnDeviceStateChanged(_Device::_OnStateChanged cb) {
    if (!cb)
      return;

    FanMain.OnStateChanged(cb);
    FanInner.OnStateChanged(cb);
    Heater.OnStateChanged(cb);
    Humidifier.OnStateChanged(cb);
  }

  _Device *operator[](_SubType subType) {
    if (FanMain.SubTypeIs(subType))
      return &FanMain;
    if (FanInner.SubTypeIs(subType))
      return &FanInner;
    if (Heater.SubTypeIs(subType))
      return &Heater;
    if (Humidifier.SubTypeIs(subType))
      return &Humidifier;

    return nullptr;
  }

  _Device *operator[](int TypeID) {
    if (FanMain.SubTypeIs(TypeID))
      return &FanMain;
    if (FanInner.SubTypeIs(TypeID))
      return &FanInner;
    if (Heater.SubTypeIs(TypeID))
      return &Heater;
    if (Humidifier.SubTypeIs(TypeID))
      return &Humidifier;

    return nullptr;
  }
};

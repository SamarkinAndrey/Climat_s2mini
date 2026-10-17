#pragma once

#include "_common.h"
#include <GyverTimer.h>

class _Device : public _UsingName, public _UsingIniciator {
private:
  GTimer _DurationTimer;
  GTimer _DelayTimer;

  int _Pin = 0;

  ulong _Duration      = 0;
  ulong _Delay         = 0;
  ulong _TotalDuration = 0;

  int _LoopCount = 0;

  ulong _HardwareDelay = 0;

  ulong _StartMillis = 0;
  ulong _StopMillis  = 0;

  uint8_t _Level = HIGH;

  int _LoopNum = 0;

  bool _PinState() {
    return digitalRead(_Pin);
  }

  void _TurnOn() {
    if (_PinState() == _Level)
      return;

    digitalWrite(_Pin, _Level);
    delay(50);

    debug.tprintf("%s._TurnOn()\n", _Name);
  }

  void _TurnOff() {
    if (_PinState() != _Level)
      return;

    digitalWrite(_Pin, !_Level);
    delay(50);

    debug.tprintf("%s._TurnOff()\n", _Name);
  }

public:
  _Device(int Pin, uint8_t Level = HIGH, ulong HardwareDelay = 0) {
    _Pin           = Pin;
    _Level         = Level;
    _HardwareDelay = HardwareDelay;

    pinMode(_Pin, OUTPUT);

    _DurationTimer.setMode(MS);
    _DelayTimer.setMode(MS);

    _TurnOff();
  }

  void Start(void *Iniciator = nullptr, ulong Duration = 0, ulong Delay = 0, int LoopCount = 0, ulong TotalDuration = 0) {
    if (IsActive() && (_Iniciator != Iniciator))
      return;

    _Duration      = (Duration > 0) ? Duration + _HardwareDelay : 0;
    _Delay         = Delay;
    _LoopCount     = LoopCount;
    _TotalDuration = TotalDuration;
    _LoopNum       = 0;

    if (IsActive()) {
      if (_Iniciator == Iniciator)
        return;
      else
        Stop();
    }

    debug.tprintf("%s.Start()\n", _Name);

    _Iniciator = Iniciator;
    _TurnOn();
    _StartMillis = millis();
    _StopMillis  = 0;

    if (_Duration > 0)
      _DurationTimer.setTimeout(_Duration);
  }

  void Stop(void *Iniciator = nullptr) {
    if (!IsActive() || (_Iniciator != Iniciator))
      return;

    if (_DurationTimer.isEnabled())
      _DurationTimer.stop();

    if (_DelayTimer.isEnabled())
      _DelayTimer.stop();

    debug.tprintf("%s.Stop()\n", _Name);

    _Iniciator = Iniciator;
    _TurnOff();
    _StartMillis = 0;
    _StopMillis  = millis();
  }

  void setDuration(ulong Value = 0) {
    if (IsActive())
      _Duration = Value;
  }

  void setDelay(ulong Value = 0) {
    if (IsActive())
      _Delay = Value;
  }

  void setLevel(uint8_t Level = HIGH) {
    _Level = Level;
  }

  uint8_t getLevel() {
    return _Level;
  }

  bool IsActive() {
    return (_StartMillis > 0);
  }

  void Tick() {
    if ((_TotalDuration > 0) && (_StartMillis > 0) && ((millis() - _StartMillis) >= _TotalDuration)) {
      Stop();
      return;
    }

    if (_DurationTimer.isReady()) {
      _TurnOff();
      if (_Delay > 0) {
        _DelayTimer.setTimeout(_Delay);
      } else {
        _LoopNum++;

        if ((_LoopCount > 0) && (_LoopNum >= _LoopCount)) {
          Stop();
          return;
        };
      }
    }
    if (_DelayTimer.isReady()) {
      _LoopNum++;

      if ((_LoopCount > 0) && (_LoopNum >= _LoopCount)) {
        Stop();
        return;
      };

      _TurnOn();
      if (_Duration > 0)
        _DurationTimer.setTimeout(_Duration);
    }
  }
};

class _DeviceList {
public:
  _Device FanMain;
  _Device FanInner;
  _Device Heater;
  _Device Humidifier;

  _DeviceList() : FanMain(FAN_MAIN_PIN),
                  FanInner(FAN_INNER_PIN),
                  Heater(HEATER_PIN),
                  Humidifier(HUMIDIFIER_PIN, LOW, HUMIDIFIER_HARDWARE_DELAY) {
    FanMain.setName("Fan(main)");
    FanInner.setName("Fan(inner)");
    Heater.setName("Heater");
    Humidifier.setName("Humidifier");
  }

  void Tick() {
    FanMain.Tick();
    FanInner.Tick();
    Heater.Tick();
    Humidifier.Tick();
  }
};

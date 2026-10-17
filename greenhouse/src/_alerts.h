#pragma once

#include "_classtype.h"
#include "_common.h"

enum class _AlertState : uint8_t {
  Idle          = 0,
  Low           = 1,
  High          = 2,
  LowNoEffect   = 3,
  HighNoEffect  = 4,
  LowSuspended  = 5,
  HighSuspended = 6
};

const char *_AlertStateName[] PROGMEM = {
    "IDLE",
    "LOW",
    "HIGH",
    "LOW_NO_EFFECT",
    "HIGH_NO_EFFECT",
    "LOW_SUSPENDED",
    "HIGH_SUSPENDED"};

const char *AlertStateName(_AlertState Value) {
  return _AlertStateName[static_cast<uint8_t>(Value)];
}

enum _AlertColors : uint32_t {
  Red       = 0xcb2839,
  Blue      = 0x297bcd,
  LightGray = 0xacacac,
  Default   = 0xffffffff
};

class _AlertList;

class _Alert : public _ClassType, public _ClassOwner<_AlertList> {
public:
  using _OnStateChanged = std::function<void(_Alert &Alert, _AlertState StateFrom, _AlertState StateTo)>;

private:
  struct _PrevAlert {
    _AlertState StartState;
    _AlertState StopState;

    ulong StartMillis;
    ulong StopMillis;

    float StartValue;
    float StopValue;

    bool IsValid;

    _PrevAlert() {
      clear();
    }

    void clear() {
      StartState = _AlertState::Idle;
      StopState  = _AlertState::Idle;

      StartMillis = 0;
      StopMillis  = 0;

      StartValue = 0;
      StopValue  = 0;

      IsValid = false;
    };
  };

  _OnStateChanged _on_state_changed = nullptr;

  _AlertState _State     = _AlertState::Idle;
  _AlertState _StatePrev = _AlertState::Idle;

  bool _IsChanged = false;

  void _StateChanged() {
    if (_on_state_changed)
      _on_state_changed(*this, _StatePrev, _State);
  }

public:
  _PrevAlert PrevAlert;

  ulong StartMillis = 0;
  float StartValue  = 0;

  _Alert(_AlertList &Alerts)
      : _ClassOwner<_AlertList>(Alerts) {
  }

  void setState(_AlertState State, float Value = 0) {
    _IsChanged = (_State != State);

    if (!_IsChanged)
      return;

    ulong _millis = millis();

    switch (State) {
      case _AlertState::Idle:
      case _AlertState::LowSuspended:
      case _AlertState::HighSuspended:
      case _AlertState::LowNoEffect:
      case _AlertState::HighNoEffect:
        switch (_State) {
          case _AlertState::Low:
          case _AlertState::High:
            PrevAlert.StartMillis = StartMillis;
            PrevAlert.StartState  = _State;
            PrevAlert.StartValue  = StartValue;
            PrevAlert.StopMillis  = _millis;
            PrevAlert.StopState   = State;
            PrevAlert.StopValue   = Value;

            PrevAlert.IsValid = true;

            StartMillis = 0;
            StartValue  = 0;
            break;
        }
        break;

      case _AlertState::Low:
      case _AlertState::High:
        PrevAlert.clear();

        StartMillis = _millis;
        StartValue  = Value;
        break;
    }

    _StatePrev = _State;
    _State     = State;

    _StateChanged();
  }

  bool StateIs(_AlertState Value) {
    return (_State == Value);
  }

  bool StateIs(std::initializer_list<_AlertState> Values) {
    for (auto v : Values) {
      if (_State == v)
        return true;
    }
    return false;
  }

  _AlertState State() {
    return _State;
  }

  _AlertState StatePrev() {
    return _StatePrev;
  }

  const char *StateName() {
    return AlertStateName(_State);
  }

  const char *StatePrevName() {
    return AlertStateName(_StatePrev);
  }

  _AlertColors Color() {
    switch (_State) {
      case _AlertState::Low:
      case _AlertState::LowNoEffect:
      case _AlertState::LowSuspended:
        return _AlertColors::Blue;
      case _AlertState::High:
      case _AlertState::HighNoEffect:
      case _AlertState::HighSuspended:
        return _AlertColors::Red;
      default:
        return _AlertColors::LightGray;
    }
  }

  bool Changed() {
    return _IsChanged;
  }

  void OnStateChanged(_OnStateChanged cb) {
    if (cb)
      _on_state_changed = cb;
  }
};

class _AlertList {
public:
  _Alert Temperature;
  _Alert Humidity;
  _Alert CO2;

public:
  _AlertList()
      : Temperature(*this),
        Humidity(*this),
        CO2(*this) {
    Temperature.setType(_MainType::Alert, _SubType::Temperature);
    Humidity.setType(_MainType::Alert, _SubType::Humidity);
    CO2.setType(_MainType::Alert, _SubType::CO2);
  }

  _Alert *operator[](_SubType subType) {
    if (Temperature.SubTypeIs(subType))
      return &Temperature;
    if (Humidity.SubTypeIs(subType))
      return &Humidity;
    if (CO2.SubTypeIs(subType))
      return &CO2;

    return nullptr;
  }

  _Alert *operator[](int TypeID) {
    if (Temperature.SubTypeIs(TypeID))
      return &Temperature;
    if (Humidity.SubTypeIs(TypeID))
      return &Humidity;
    if (CO2.SubTypeIs(TypeID))
      return &CO2;

    return nullptr;
  }

  void OnAlertStateChanged(_Alert::_OnStateChanged cb) {
    if (!cb)
      return;

    Temperature.OnStateChanged(cb);
    Humidity.OnStateChanged(cb);
    CO2.OnStateChanged(cb);
  }
};

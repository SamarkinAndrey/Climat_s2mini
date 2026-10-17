#pragma once

#include "_common.h"

enum _AlertType {
  AT_NONE,
  AT_TEMPERATURE,
  AT_HUMIDITY,
  AT_CO2
};

enum _AlertState {
  AS_NONE,
  AS_COMPLETE,
  AS_OUTER_SENSOR,
  AS_OUTER_VALUE,
  AS_LOW,
  AS_HIGH
};

enum _AlertColors : uint32_t {
  AC_RED       = 0xcb2839,
  AC_BLUE      = 0x297bcd,
  AC_LIGHTGRAY = 0xacacac,
  AC_DEFAULT   = 0xffffffff
};

class _Alert {
private:
  struct _PrevAlert {
    _AlertState StartState;
    _AlertState StopState;

    ulong StartMillis;
    ulong StopMillis;

    float StartValue;
    float StopValue;

    _PrevAlert() {
      Clear();
    }

    void Clear() {
      StartState = AS_NONE;
      StopState  = AS_NONE;

      StartMillis = 0;
      StopMillis  = 0;

      StartValue = 0;
      StopValue  = 0;
    };
  };

  _AlertType  _Type  = AT_NONE;
  _AlertState _State = AS_NONE;

public:
  _PrevAlert PrevAlert;

  ulong StartMillis = 0;
  float StartValue  = 0;

  _Alert(_AlertType Type) {
    _Type = Type;
  }

  void setState(_AlertState State, float Value = 0) {
    if (_State == State)
      return;

    ulong _millis = millis();

    switch (State) {
      case (AS_NONE):
      case (AS_COMPLETE):
      case (AS_OUTER_SENSOR):
      case (AS_OUTER_VALUE):
        switch (_State) {
          case (AS_LOW):
          case (AS_HIGH):
            PrevAlert.StartMillis = StartMillis;
            PrevAlert.StartState  = _State;
            PrevAlert.StartValue  = StartValue;
            PrevAlert.StopMillis  = _millis;
            PrevAlert.StopState   = State;
            PrevAlert.StopValue   = Value;

            StartMillis = 0;
            StartValue  = 0;
            break;
        }
        break;

      case (AS_LOW):
      case (AS_HIGH):
        PrevAlert.Clear();

        StartMillis = _millis;
        StartValue  = Value;
        break;
    }

    _State = State;
  }

  _AlertState getState() {
    return _State;
  }

  _AlertType getType() {
    return _Type;
  }

  _AlertColors getColor() {
    switch (_State) {
      case AS_HIGH:
        return AC_RED;
      case AS_LOW:
        return AC_BLUE;
      default:
        return AC_LIGHTGRAY;
    }
  }
};

class _AlertList {
public:
  _Alert Temperature;
  _Alert Humidity;
  _Alert CO2;

public:
  _AlertList() : Temperature(AT_TEMPERATURE),
                 Humidity(AT_HUMIDITY),
                 CO2(AT_CO2) {
  }
};

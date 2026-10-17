#pragma once

#include "_common.h"
#include "_classtype.h"
#include <Arduino.h>
#include <PID_v1.h>

#define PID_WINDOW_MIN      60000ul
#define PID_SAMPLE_MIN      10000ul
#define PID_DUTY_MIN_ON     2000ul
#define PID_DUTY_MIN_OFF    2000ul
#define PID_WINDOW_DEFAULT  300000ul
#define PID_SAMPLE_DEFAULT  20000ul

class _PIDControl : public _ClassType {
private:
  double _Input    = 0;
  double _Output   = 0;
  double _Setpoint = 0;

  PID _PID;

  ulong _SampleTime = 60000ul;
  ulong _Window     = 600000ul;

  ulong _OnTime  = 0;
  ulong _OffTime = 0;

  float _HeatDuty = 0.0f;
  float _CoolDuty = 0.0f;

  ulong _DutyMinOn  = PID_DUTY_MIN_ON;
  ulong _DutyMinOff = PID_DUTY_MIN_OFF;

  bool _LastComputeValid = false;

  void _updateDuty() {
    float _heat = (float)_Output / 100.0f;
    if (_heat < 0.0f)
      _heat = 0.0f;
    if (_heat > 1.0f)
      _heat = 1.0f;

    float _cool = (float)(-_Output) / 100.0f;
    if (_cool < 0.0f)
      _cool = 0.0f;
    if (_cool > 1.0f)
      _cool = 1.0f;

    _HeatDuty = _heat;
    _CoolDuty = _cool;

    _OnTime  = (ulong)(_Window * _heat);
    _OffTime = _Window - _OnTime;

    if (_OnTime > 0 && _OnTime < _DutyMinOn)
      _OnTime = _DutyMinOn;

    if (_OffTime > 0 && _OffTime < _DutyMinOff)
      _OffTime = _DutyMinOff;
  }

public:
  _PIDControl(_SubType subType = _SubType::Undefined)
      : _ClassType(_MainType::Undefined, subType),
        _PID(&_Input, &_Output, &_Setpoint, 0.0, 0.0, 0.0, P_ON_M, DIRECT) {
    _PID.SetOutputLimits(0, 100);
    _PID.SetSampleTime((int)_SampleTime);
    _PID.SetMode(AUTOMATIC);
  }

  void setTunings(float Kp, float Ki, float Kd) {
    _PID.SetTunings((double)Kp, (double)Ki, (double)Kd);
  }

  void setOutputLimits(double Min, double Max) {
    _PID.SetOutputLimits(Min, Max);
    _updateDuty();
  }

  void setSetpoint(float Setpoint) {
    _Setpoint = (double)Setpoint;
  }

  void setSampleTime(ulong SampleTime) {
    if (SampleTime < PID_SAMPLE_MIN)
      SampleTime = PID_SAMPLE_MIN;
    _SampleTime = SampleTime;
    _PID.SetSampleTime((int)_SampleTime);
  }

  void setWindow(ulong Window) {
    if (Window < PID_WINDOW_MIN)
      Window = PID_WINDOW_MIN;
    _Window = Window;
    _updateDuty();
  }

  void setDutyMin(ulong MinOn, ulong MinOff) {
    _DutyMinOn  = (MinOn > 0) ? MinOn : PID_DUTY_MIN_ON;
    _DutyMinOff = (MinOff > 0) ? MinOff : PID_DUTY_MIN_OFF;
    _updateDuty();
  }

  float getOutputPercent() const {
    return (float)_Output;
  }

  float getHeatDuty() const {
    return _HeatDuty;
  }

  float getCoolDuty() const {
    return _CoolDuty;
  }

  ulong getOnTime() const {
    return _OnTime;
  }

  ulong getOffTime() const {
    return _OffTime;
  }

  bool LastComputeValid() const {
    return _LastComputeValid;
  }

  bool Tick(float ProcessValue, bool IsValid) {
    if (!IsValid) {
      _PID.SetMode(MANUAL);
      _Output = 0.0;
      _OnTime = 0;
      _OffTime = 0;
      _HeatDuty = 0.0f;
      _CoolDuty = 0.0f;
      _LastComputeValid = false;
      return false;
    }

    _PID.SetMode(AUTOMATIC);
    _Input = (double)ProcessValue;

    if (!_PID.Compute()) {
      _LastComputeValid = false;
      return false;
    }

    _updateDuty();
    _LastComputeValid = true;

    debug.tprintf("PID %s: PV=%.2f SP=%.2f out=%.1f%% heat=%.1f%% cool=%.1f%% on=%lu off=%lu\n",
                  Name(),
                  (float)_Input,
                  (float)_Setpoint,
                  (float)_Output,
                  _HeatDuty * 100.0f,
                  _CoolDuty * 100.0f,
                  _OnTime,
                  _OffTime);

    return true;
  }
};

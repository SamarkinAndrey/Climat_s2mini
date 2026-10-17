#include "_alerts.h"
#include "_common.h"
#include "_controller.h"
#include "_devices.h"
#include "_greenhouse.h"
#include "_params_state.h"
#include "_pid.h"
#include "_rdp.h"
#include "_sensors.h"

_Plot p(_SubType::Humidity);

_ParamState<bool> Params;

_GreenHouse GreenHouse;

DeviceController HC(_MainType::Device, _SubType::Humidifier, 10000ul, 20000ul);

_PIDControl PIDHumidity(_SubType::Humidity);

inline void PIDHumidityUpdate() {
  float _low  = db[dbParams::HumidityAlarmThresholdLow].toFloat();
  float _high = db[dbParams::HumidityAlarmThresholdHigh].toFloat();

  if (_low > _high) {
    float _swap = _low;
    _low  = _high;
    _high = _swap;
  }

  float _setpoint = db[dbParams::HumiditySetpoint].toFloat();
  if (_setpoint < _low)
    _setpoint = _low;
  if (_setpoint > _high)
    _setpoint = _high;

  PIDHumidity.setSetpoint(_setpoint);
  PIDHumidity.setTunings(db[dbParams::HumidityKp].toFloat(), db[dbParams::HumidityKi].toFloat(), db[dbParams::HumidityKd].toFloat());
}

_PIDControl PIDCO2(_SubType::CO2);

inline float CO2Diff() {
  float _out = GreenHouse.Sensors.mhz19out.CO2.Value();
  if (_out < 1)
    return GreenHouse.Sensors.mhz19in.CO2.Value();
  return GreenHouse.Sensors.mhz19in.CO2.Value() - _out;
}

inline void PIDCO2Update() {
  PIDCO2.setSetpoint(db[dbParams::CO2PidSetpoint].toFloat());
  PIDCO2.setTunings(db[dbParams::CO2PidKp].toFloat(), db[dbParams::CO2PidKi].toFloat(), db[dbParams::CO2PidKd].toFloat());
}

_PIDControl PIDTemperature(_SubType::Temperature);

inline void PIDTemperatureUpdate() {
  float _low  = db[dbParams::TemperatureAlarmThresholdLow].toFloat();
  float _high = db[dbParams::TemperatureAlarmThresholdHigh].toFloat();

  if (_low > _high) {
    float _swap = _low;
    _low  = _high;
    _high = _swap;
  }

  float _setpoint = db[dbParams::TemperatureSetpoint].toFloat();
  if (_setpoint < _low)
    _setpoint = _low;
  if (_setpoint > _high)
    _setpoint = _high;

  PIDTemperature.setSetpoint(_setpoint);
  PIDTemperature.setTunings(db[dbParams::TemperatureKp].toFloat(), db[dbParams::TemperatureKi].toFloat(), db[dbParams::TemperatureKd].toFloat());
}

struct _localParams {
  int  mhz19in_Range;
  int  mhz19out_Range;
  bool mhz19in_AutoCalibration;
  bool mhz19out_AutoCalibration;
};

_localParams localParams;

inline void WebUpdate() {
  // debug.tprintln("WebUpdate()");

  sett.updater()
      .update(dbParams::TemperatureIn, GreenHouse.Sensors.dht22in.Temperature.Text())
      .update(dbParams::HumidityIn, GreenHouse.Sensors.dht22in.Humidity.Text())
      .update(dbParams::CO2In, GreenHouse.Sensors.mhz19in.CO2.Text())
      .update(dbParams::TemperatureOut, GreenHouse.Sensors.dht22out.Temperature.Text())
      .update(dbParams::HumidityOut, GreenHouse.Sensors.dht22out.Humidity.Text())
      .update(dbParams::CO2Out, GreenHouse.Sensors.mhz19out.CO2.Text())
      .update(dbParams::HumidifierLED, GreenHouse.Devices.Humidifier.State())
      .update(dbParams::HeaterLED, GreenHouse.Devices.Heater.State())
      .update(dbParams::FanMainLED, GreenHouse.Devices.FanMain.State())
      .update(dbParams::FanInnerLED, GreenHouse.Devices.FanInner.State())
      .update(H(log), logger);

  if (PIDHumidity.LastComputeValid()) {
    String _pidDuty = String(PIDHumidity.getOutputPercent(), 1) + " % (" + String(PIDHumidity.getOnTime() / 1000ul) + "/" + String(PIDHumidity.getOffTime() / 1000ul) + " сек)";
    sett.updater().update(dbParams::HumidityPidDuty, _pidDuty);
  }

  if (PIDTemperature.LastComputeValid()) {
    String _pidDuty = String(PIDTemperature.getHeatDuty() * 100.0f, 1) + "/" + String(PIDTemperature.getCoolDuty() * 100.0f, 1) + " %";
    sett.updater().update(dbParams::TemperaturePidDuty, _pidDuty);
  }

  if (PIDCO2.LastComputeValid()) {
    String _pidDuty = String(PIDCO2.getOutputPercent(), 1) + " % (" + String(PIDCO2.getOnTime() / 1000ul) + "/" + String(PIDCO2.getOffTime() / 1000ul) + " сек)";
    sett.updater().update(dbParams::CO2PidDuty, _pidDuty);
  }

  if (!db[TemperatureModeIn].toBool()) {
    sett.updater().updatePlot<float>(dbParams::TemperaturePlotIn, {GreenHouse.Sensors.dht22in.Temperature.Value()});
  }

  // if (!db[HumidityModeIn].toBool()) {
  //   sett.updater().updatePlot<float>(dbParams::HumidityPlotIn, {GreenHouse.Sensors.dht22in.Humidity.Value()});
  // }

  if (!db[HumidityModeIn].toBool()) {
    p.putValue(GreenHouse.Sensors.dht22in.Humidity.Value());

    sett.updater().updatePlot(dbParams::HumidityPlotIn, p.Point(), true);
  }

  if (!db[CO2ModeIn].toBool()) {
    sett.updater().updatePlot<float>(dbParams::CO2PlotIn, {GreenHouse.Sensors.mhz19in.CO2.Value()});
  }

  if (GreenHouse.Sync.SubTypeIs(_SubType::Temperature))
    sett.updater().update(dbParams::TemperatureSyncPercent, GreenHouse.Sync.MillisLeftStr());

  if (GreenHouse.Sync.SubTypeIs(_SubType::Humidity))
    sett.updater().update(dbParams::HumiditySyncPercent, GreenHouse.Sync.MillisLeftStr());

  if (GreenHouse.Sync.SubTypeIs(_SubType::CO2))
    sett.updater().update(dbParams::CO2SyncPercent, GreenHouse.Sync.MillisLeftStr());

  sett.updater().updateColor(dbParams::TemperatureIn, GreenHouse.Alerts.Temperature.Color());
  sett.updater().updateColor(dbParams::HumidityIn, GreenHouse.Alerts.Humidity.Color());
  sett.updater().updateColor(dbParams::CO2In, GreenHouse.Alerts.CO2.Color());

  Params[dbParams::DHT22InExists]  = GreenHouse.Sensors.dht22in.IsValid();
  Params[dbParams::DHT22OutExists] = GreenHouse.Sensors.dht22out.IsValid();
  Params[dbParams::MHZ19InExists]  = GreenHouse.Sensors.mhz19in.IsValid();
  Params[dbParams::MHZ19OutExists] = GreenHouse.Sensors.mhz19out.IsValid();

  Params[dbParams::MHZ19InHeating]  = GreenHouse.Sensors.mhz19in.IsHeating();
  Params[dbParams::MHZ19OutHeating] = GreenHouse.Sensors.mhz19out.IsHeating();

  Params[dbParams::TemperatureIsSync] = GreenHouse.Sync.SubTypeIs(_SubType::Temperature);
  Params[dbParams::HumidityIsSync]    = GreenHouse.Sync.SubTypeIs(_SubType::Humidity);
  Params[dbParams::CO2IsSync]         = GreenHouse.Sync.SubTypeIs(_SubType::CO2);

  Params[dbParams::TemperatureModeIn] = db[TemperatureModeIn];
  Params[dbParams::HumidityModeIn]    = db[HumidityModeIn];
  Params[dbParams::CO2ModeIn]         = db[CO2ModeIn];

  Params[dbParams::TemperatureModeOut] = db[TemperatureModeOut];
  Params[dbParams::HumidityModeOut]    = db[HumidityModeOut];
  Params[dbParams::CO2ModeOut]         = db[CO2ModeOut];

  static ulong _reloadMillis  = 0;
  static bool  _reloadPending = false;

  if (Params.IsChanged())
    _reloadPending = true;

  if (_reloadPending &&
      ((_reloadMillis == 0) || ((millis() - _reloadMillis) >= 3000ul))) {
    _reloadPending = false;
    _reloadMillis  = millis();

    debug.tprintln("Params.IsChanged()");

    sett.reload();
  }
}

inline void AlertsUpdate() {
  //  debug.tprintln("AlertsUpdate()");

  _Alert &Temperature = GreenHouse.Alerts.Temperature;
  _Alert &Humidity    = GreenHouse.Alerts.Humidity;
  _Alert &CO2         = GreenHouse.Alerts.CO2;

  _Readings &TemperatureIn = GreenHouse.Sensors.dht22in.Temperature;
  _Readings &HumidityIn    = GreenHouse.Sensors.dht22in.Humidity;
  _Readings &CO2In         = GreenHouse.Sensors.mhz19in.CO2;

  _Readings &TemperatureOut = GreenHouse.Sensors.dht22out.Temperature;
  _Readings &HumidityOut    = GreenHouse.Sensors.dht22out.Humidity;
  _Readings &CO2Out         = GreenHouse.Sensors.mhz19out.CO2;

  _Device &FanMain    = GreenHouse.Devices.FanMain;
  _Device &FanInner   = GreenHouse.Devices.FanInner;
  _Device &Humidifier = GreenHouse.Devices.Humidifier;
  _Device &Heater     = GreenHouse.Devices.Heater;

  float _midTemperature = (db[TemperatureAlarmThresholdHigh].toFloat() + db[TemperatureAlarmThresholdLow].toFloat()) / 2;
  float _midHumidity    = (db[HumidityAlarmThresholdHigh].toFloat() + db[HumidityAlarmThresholdLow].toFloat()) / 2;
  float _midCO2         = db[CO2AlarmThresholdHigh].toFloat() - (db[CO2Hysteresis].toFloat() / 2);

  if (db[TemperatureControlEnabled].toBool() && GreenHouse.Sensors.dht22in.IsValid()) {
    switch (Temperature.State()) {
      case _AlertState::Idle:
        if (TemperatureIn.Value() < db[TemperatureAlarmThresholdLow].toFloat())
          Temperature.setState(_AlertState::Low);

        if ((TemperatureIn.Value() > db[TemperatureAlarmThresholdHigh].toFloat()) &&
            ((TemperatureOut.Value() < 1) || TemperatureIn.GreaterThen(TemperatureOut)))
          Temperature.setState(_AlertState::High);

        break;

      case _AlertState::Low:
        if (TemperatureIn.Value() >= _midTemperature) {
          Temperature.setState(_AlertState::Idle);
        } else {
          if (!Heater.IsWorking() &&
              !TemperatureIn.IsUp() &&
              (TemperatureIn.IsUpBy() < db[TemperatureHeatingEffectiveThreshold].toFloat())) {
            //
            if (TemperatureIn.Value() < db[TemperatureAlarmThresholdLow].toFloat())
              Temperature.setState(_AlertState::LowNoEffect);
            else
              Temperature.setState(_AlertState::Idle);

            debug.tprintf("%s.IsUpBy: %.2f\n", TemperatureIn.Name(), TemperatureIn.IsUpBy());
            debug.tprintf("%s.Diff: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.Diff);
            debug.tprintf("%s.From: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.ValueFrom);
            debug.tprintf("%s.To: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.ValueTo);
          }
        }

        break;

      case _AlertState::High:
        if ((TemperatureIn.Value() <= _midTemperature) ||
            ((TemperatureOut.Value() > 0) && TemperatureIn.SmallerOrEqualTo(TemperatureOut))) {
          Temperature.setState(_AlertState::Idle);
        } else {
          if (!FanMain.IsWorking() &&
              !TemperatureIn.IsDown() &&
              (TemperatureIn.IsDownBy() < db[TemperatureFanEffectiveThreshold].toFloat())) {
            //
            if ((TemperatureIn.Value() > db[TemperatureAlarmThresholdHigh].toFloat()) &&
                ((TemperatureOut.Value() < 1) || TemperatureIn.GreaterThen(TemperatureOut)))
              Temperature.setState(_AlertState::HighNoEffect);
            else
              Temperature.setState(_AlertState::Idle);

            debug.tprintf("%s.IsDownBy: %.2f\n", TemperatureIn.Name(), TemperatureIn.IsDownBy());
            debug.tprintf("%s.Diff: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.Diff);
            debug.tprintf("%s.From: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.ValueFrom);
            debug.tprintf("%s.To: %.2f\n", TemperatureIn.Name(), TemperatureIn.Stat.ValueTo);
          }
        }

        break;

      case _AlertState::LowNoEffect:
        if ((db[TemperatureHeatingNoEffectDelay].toInt() > 0) &&
            ((millis() - Temperature.PrevAlert.StopMillis) > (db[TemperatureHeatingNoEffectDelay].toInt() * 60000ul))) {
          if (TemperatureIn.Value() < db[TemperatureAlarmThresholdLow].toFloat())
            Temperature.setState(_AlertState::Low);
          else
            Temperature.setState(_AlertState::Idle);
        }

        break;

      case _AlertState::HighNoEffect:
        if ((db[TemperatureFanNoEffectDelay].toInt() > 0) &&
            ((millis() - Temperature.PrevAlert.StopMillis) > (db[TemperatureFanNoEffectDelay].toInt() * 60000ul))) {
          if ((TemperatureIn.Value() > db[TemperatureAlarmThresholdHigh].toFloat()) &&
              ((TemperatureOut.Value() < 1) || TemperatureIn.GreaterThen(TemperatureOut)))
            Temperature.setState(_AlertState::High);
          else
            Temperature.setState(_AlertState::Idle);
        }

        break;
    }
  } else {
    Temperature.setState(_AlertState::Idle);
  }

  if (db[HumidityControlEnabled].toBool() && GreenHouse.Sensors.dht22in.IsValid()) {
    switch (Humidity.State()) {
      case _AlertState::Idle:
        if (!Temperature.StateIs(_AlertState::High) &&
            !CO2.StateIs(_AlertState::High) &&
            (HumidityIn.Value() < db[HumidityAlarmThresholdLow].toFloat()))
          Humidity.setState(_AlertState::Low);

        if ((HumidityIn.Value() > db[HumidityAlarmThresholdHigh].toFloat()) &&
            ((HumidityOut.Value() < 1) || HumidityIn.GreaterThen(HumidityOut)))
          Humidity.setState(_AlertState::High);

        break;

      case _AlertState::LowSuspended:
        if (HumidityIn.Value() >= _midHumidity) {
          Humidity.setState(_AlertState::Idle);
        } else {
          if (!Temperature.StateIs(_AlertState::High) &&
              !CO2.StateIs(_AlertState::High) &&
              (HumidityIn.Value() < db[HumidityAlarmThresholdLow].toFloat()))
            Humidity.setState(_AlertState::Low);
        }

        break;

      case _AlertState::Low:
        if (HumidityIn.Value() >= _midHumidity) {
          Humidity.setState(_AlertState::Idle);
        } else {
          if (!Humidifier.IsWorking() &&
              !HumidityIn.IsUp() &&
              (HumidityIn.IsUpBy() < db[HumidityWettingEffectiveThreshold].toFloat())) {
            //
            if (Temperature.StateIs(_AlertState::High) ||
                CO2.StateIs(_AlertState::High))
              Humidity.setState(_AlertState::LowSuspended);
            else if (HumidityIn.Value() < db[HumidityAlarmThresholdLow].toFloat())
              Humidity.setState(_AlertState::LowNoEffect);
            else
              Humidity.setState(_AlertState::Idle);

            HC.put(HumidityIn.Stat.ValueFrom,
                   HumidityIn.Stat.ValueTo,
                   Humidifier.Duration(),
                   true);

            debug.tprintf("ValueFrom = %.2f\n", HumidityIn.Stat.ValueFrom);
            debug.tprintf("ValueTo = %.2f\n", HumidityIn.Stat.ValueTo);
            debug.tprintf("Duration = %lu\n", Humidifier.Duration());

            debug.tprintf("%s.IsUpBy: %.2f\n", HumidityIn.Name(), HumidityIn.IsUpBy());
            debug.tprintf("%s.Diff: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.Diff);
            debug.tprintf("%s.From: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.ValueFrom);
            debug.tprintf("%s.To: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.ValueTo);
          }
        }

        break;

      case _AlertState::High:
        if ((HumidityIn.Value() <= _midHumidity) ||
            ((HumidityOut.Value() > 0) && HumidityIn.SmallerOrEqualTo(HumidityOut))) {
          Humidity.setState(_AlertState::Idle);
        } else {
          if (!FanMain.IsWorking() &&
              !HumidityIn.IsDown() &&
              (HumidityIn.IsDownBy() < db[HumidityFanEffectiveThreshold].toFloat())) {
            //
            if ((HumidityIn.Value() > db[HumidityAlarmThresholdHigh].toFloat()) &&
                ((HumidityOut.Value() < 1) || HumidityIn.GreaterThen(HumidityOut)))
              Humidity.setState(_AlertState::HighNoEffect);
            else
              Humidity.setState(_AlertState::Idle);

            debug.tprintf("%s.IsDownBy: %.2f\n", HumidityIn.Name(), HumidityIn.IsDownBy());
            debug.tprintf("%s.Diff: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.Diff);
            debug.tprintf("%s.From: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.ValueFrom);
            debug.tprintf("%s.To: %.2f\n", HumidityIn.Name(), HumidityIn.Stat.ValueTo);
          }
        }

        break;

      case _AlertState::LowNoEffect:
        if ((db[HumidityWettingNoEffectDelay].toInt() > 0) &&
            ((millis() - Humidity.PrevAlert.StopMillis) > (db[HumidityWettingNoEffectDelay].toInt() * 60000ul))) {
          if (HumidityIn.Value() < db[HumidityAlarmThresholdLow].toFloat())
            Humidity.setState(_AlertState::Low);
          else
            Humidity.setState(_AlertState::Idle);
        }

        break;

      case _AlertState::HighNoEffect:
        if ((db[HumidityFanNoEffectDelay].toInt() > 0) &&
            ((millis() - Humidity.PrevAlert.StopMillis) > (db[HumidityFanNoEffectDelay].toInt() * 60000ul))) {
          if ((HumidityIn.Value() > db[HumidityAlarmThresholdHigh].toFloat()) &&
              ((HumidityOut.Value() < 1) || HumidityIn.GreaterThen(HumidityOut)))
            Humidity.setState(_AlertState::High);
          else
            Humidity.setState(_AlertState::Idle);
        }

        break;
    }
  } else {
    Humidity.setState(_AlertState::Idle);
  }

  if (db[CO2ControlEnabled].toBool() && GreenHouse.Sensors.mhz19in.IsValid()) {
    switch (CO2.State()) {
      case _AlertState::Idle:
        if (CO2Diff() > db[CO2AlarmThresholdHigh].toFloat())
          CO2.setState(_AlertState::High);

        break;

      case _AlertState::High:
        if (CO2Diff() <= _midCO2) {
          CO2.setState(_AlertState::Idle);
        } else {
          if (!FanMain.IsWorking() &&
              !CO2In.IsDown() &&
              (CO2In.IsDownBy() < db[CO2FanEffectiveThreshold].toFloat())) {
            //
            if (CO2Diff() > db[CO2AlarmThresholdHigh].toFloat())
              CO2.setState(_AlertState::HighNoEffect);
            else
              CO2.setState(_AlertState::Idle);

            debug.tprintf("%s.IsDownBy: %.2f\n", CO2In.Name(), CO2In.IsDownBy());
            debug.tprintf("%s.Diff: %.2f\n", CO2In.Name(), CO2In.Stat.Diff);
            debug.tprintf("%s.From: %.2f\n", CO2In.Name(), CO2In.Stat.ValueFrom);
            debug.tprintf("%s.To: %.2f\n", CO2In.Name(), CO2In.Stat.ValueTo);
          }
        }

        break;

      case _AlertState::HighNoEffect:
        if ((db[CO2FanNoEffectDelay].toInt() > 0) &&
            ((millis() - CO2.PrevAlert.StopMillis) > (db[CO2FanNoEffectDelay].toInt() * 60000ul))) {
          if (CO2Diff() > db[CO2AlarmThresholdHigh].toFloat())
            CO2.setState(_AlertState::High);
          else
            CO2.setState(_AlertState::Idle);
        }

        break;
    }
  } else {
    CO2.setState(_AlertState::Idle);
  }

  TemperatureIn.setStatEnabled(&Temperature, Temperature.StateIs({_AlertState::Low, _AlertState::High}));
  HumidityIn.setStatEnabled(&Humidity, Humidity.StateIs({_AlertState::Low, _AlertState::High}));
  CO2In.setStatEnabled(&CO2, CO2.StateIs(_AlertState::High));
}

inline void DevicesUpdate() {
  //  debug.tprintln("DevicesUpdate()");

  _Device &FanMain    = GreenHouse.Devices.FanMain;
  _Device &FanInner   = GreenHouse.Devices.FanInner;
  _Device &Humidifier = GreenHouse.Devices.Humidifier;
  _Device &Heater     = GreenHouse.Devices.Heater;

  _Alert &Temperature = GreenHouse.Alerts.Temperature;
  _Alert &Humidity    = GreenHouse.Alerts.Humidity;
  _Alert &CO2         = GreenHouse.Alerts.CO2;

  _Readings &TemperatureIn = GreenHouse.Sensors.dht22in.Temperature;
  _Readings &HumidityIn    = GreenHouse.Sensors.dht22in.Humidity;
  _Readings &CO2In         = GreenHouse.Sensors.mhz19in.CO2;

  _Readings &TemperatureOut = GreenHouse.Sensors.dht22out.Temperature;
  _Readings &HumidityOut    = GreenHouse.Sensors.dht22out.Humidity;
  _Readings &CO2Out         = GreenHouse.Sensors.mhz19out.CO2;

  float _midTemperature = (db[TemperatureAlarmThresholdHigh].toFloat() + db[TemperatureAlarmThresholdLow].toFloat()) / 2;
  float _midHumidity    = (db[HumidityAlarmThresholdHigh].toFloat() + db[HumidityAlarmThresholdLow].toFloat()) / 2;
  float _midCO2         = db[CO2AlarmThresholdHigh].toFloat();

  switch (Temperature.State()) {
    case _AlertState::High:
      FanInner.Start(&Temperature);

      if (!FanMain.IsWorking() &&
          !TemperatureIn.IsDown(5000)) {
        TemperatureIn.Stat.Reload();
        FanMain.Start(&Temperature,
                      max(1000ul, db[TemperatureFanDuration].toInt() * 1000ul),
                      db[TemperatureFanDelay].toInt() * 1000ul,
                      1);
      }
      break;

    case _AlertState::Low:
      if (!CO2.StateIs(_AlertState::High) &&
          !Heater.IsWorking() &&
          !TemperatureIn.IsUp(5000)) {
        TemperatureIn.Stat.Reload();
        Heater.Start(&Temperature,
                     max(60000ul, db[TemperatureHeatingDuration].toInt() * 60ul * 1000ul),
                     db[TemperatureHeatingDelay].toInt() * 60ul * 1000ul,
                     1);
      }
      break;

    default:
      FanMain.Stop(&Temperature);
      Heater.Stop(&Temperature);
      FanInner.Stop(&Temperature);
  }

  switch (Humidity.State()) {
    case _AlertState::High:
      FanInner.Start(&Humidity);

      if (!FanMain.IsWorking() &&
          !HumidityIn.IsDown(5000)) {
        HumidityIn.Stat.Reload();
        FanMain.Start(&Humidity,
                      max(1000ul, db[HumidityFanDuration].toInt() * 1000ul),
                      db[HumidityFanDelay].toInt() * 1000ul,
                      1);
      }
      break;

    case _AlertState::Low:
      FanInner.Start(&Humidity);

      if (!Temperature.StateIs(_AlertState::High) &&
          !CO2.StateIs(_AlertState::High) &&
          !Humidifier.IsWorking() &&
          !HumidityIn.IsUp(5000)) {
        if (HumidityIn.Stat.IsValid) {
          HC.put(HumidityIn.Stat.ValueFrom,
                 HumidityIn.Stat.ValueTo,
                 Humidifier.Duration(),
                 true);

          debug.tprintf("ValueFrom = %.2f\n", HumidityIn.Stat.ValueFrom);
          debug.tprintf("ValueTo = %.2f\n", HumidityIn.Stat.ValueTo);
          debug.tprintf("Duration = %lu\n", Humidifier.Duration());
        }

        HumidityIn.Stat.Reload();

        float value   = min(3.0f, _midHumidity - HumidityIn.Value());
        ulong predict = HC.predict(HumidityIn.Value(), HumidityIn.Value() + value);

        debug.tprintf("predict = %lu\n", predict);

        if (predict < 1)
          predict = max(1000ul, db[HumidityWettingDuration].toInt() * 1000ul);

        Humidifier.Start(&Humidity,
                         //  db[HumidityWettingDuration].toInt() * 1000ul,
                         predict,
                         db[HumidityWettingDelay].toInt() * 1000ul,
                         1);
      }
      break;

    default:
      FanMain.Stop(&Humidity);
      Humidifier.Stop(&Humidity);
      FanInner.Stop(&Humidity);
  }

  switch (CO2.State()) {
    case _AlertState::High:
      FanInner.Start(&CO2);

      if (!FanMain.IsWorking() &&
          !CO2In.IsDown(5000)) {
        CO2In.Stat.Reload();
        FanMain.Start(&CO2,
                      max(1000ul, db[CO2FanDuration].toInt() * 1000ul),
                      db[CO2FanDelay].toInt() * 1000ul,
                      1);
      }
      break;

    default:
      FanMain.Stop(&CO2);
      FanInner.Stop(&CO2);
  }
}

inline void WebAction(const size_t Param, const Text Value) {
  if (Param < (sizeof(dbParamsName) / sizeof(char *))) {
    debug.tprint(dbParamsName[Param]);
  } else {
    debug.tprint(Param);
  }
  debug.print(" = ");
  debug.println(Value.toString());

  if ((Param == dbParams::TemperatureControlEnabled) ||
      (Param == dbParams::HumidityControlEnabled) ||
      (Param == dbParams::CO2ControlEnabled)) {
    GreenHouse.AlertsUpdate();
  }

  switch (Param) {
    case dbParams::DebugMode:
      debug.setModeInt(Value.toInt());
      break;

    case dbParams::SensorScanDelay:
      GreenHouse.setSensorUpdateInterval(Value.toInt() * 1000ul);
      break;

    case dbParams::TemperatureInStackSize:
      GreenHouse.Sensors.dht22in.setTemperatureStackSize(Value.toInt());
      break;

    case dbParams::HumidityInStackSize:
      GreenHouse.Sensors.dht22in.setHumidityStackSize(Value.toInt());
      break;

    case dbParams::CO2InStackSize:
      GreenHouse.Sensors.mhz19in.setCO2StackSize(Value.toInt());
      break;

    case dbParams::TemperatureOutStackSize:
      GreenHouse.Sensors.dht22out.setTemperatureStackSize(Value.toInt());
      break;

    case dbParams::HumidityOutStackSize:
      GreenHouse.Sensors.dht22out.setHumidityStackSize(Value.toInt());
      break;

    case dbParams::CO2OutStackSize:
      GreenHouse.Sensors.mhz19out.setCO2StackSize(Value.toInt());
      break;

    case dbParams::TemperatureOffset:
      GreenHouse.Sensors.dht22out.setTemperatureOffset(Value.toFloat());
      break;

    case dbParams::HumidityOffset:
      GreenHouse.Sensors.dht22out.setHumidityOffset(Value.toFloat());
      break;

    case dbParams::CO2Offset:
      GreenHouse.Sensors.mhz19out.setCO2Offset(Value.toFloat());
      break;

    case dbParams::TemperatureHysteresis:
      GreenHouse.Sensors.dht22in.setTemperatureHysteresis(Value.toFloat());
      break;

    case dbParams::HumidityHysteresis:
      GreenHouse.Sensors.dht22in.setHumidityHysteresis(Value.toFloat());
      break;

    case dbParams::CO2Hysteresis:
      GreenHouse.Sensors.mhz19in.setCO2Hysteresis(Value.toFloat());
      break;

    case dbParams::CO2InRange:
      // GreenHouse.Sensors.mhz19in.setRange(Value.toInt() == 0 ? 2000 : 5000);
      break;

    case dbParams::CO2InAutoCalibration:
      // GreenHouse.Sensors.mhz19in.setAutoCalibration(Value.toBool());
      break;

    case dbParams::CO2OutRange:
      // GreenHouse.Sensors.mhz19out.setRange(Value.toInt() == 0 ? 2000 : 5000);
      break;

    case dbParams::CO2OutAutoCalibration:
      // GreenHouse.Sensors.mhz19out.setAutoCalibration(Value.toBool());
      break;

    case dbParams::MqttServer:
      mqtt.setServer(Value.toString().c_str());
      break;

    case dbParams::MqttPort:
      mqtt.setPort(Value.toInt());
      break;

    case dbParams::MqttUser:
      mqtt.setUser(Value.toString().c_str());
      break;

    case dbParams::MqttPassword:
      mqtt.setPassword(Value.toString().c_str());
      break;

    case dbParams::MqttPublishDelay:
      mqtt.setPublishDelay(Value.toInt() * 1000ul);
      break;

    case dbParams::HumidityKp:
    case dbParams::HumidityKi:
    case dbParams::HumidityKd:
    case dbParams::HumiditySetpoint:
    case dbParams::HumidityAlarmThresholdLow:
    case dbParams::HumidityAlarmThresholdHigh:
      PIDHumidityUpdate();
      break;

    case dbParams::TemperatureKp:
    case dbParams::TemperatureKi:
    case dbParams::TemperatureKd:
    case dbParams::TemperatureSetpoint:
    case dbParams::TemperatureAlarmThresholdLow:
    case dbParams::TemperatureAlarmThresholdHigh:
      PIDTemperatureUpdate();
      break;

    case dbParams::CO2PidKp:
    case dbParams::CO2PidKi:
    case dbParams::CO2PidKd:
    case dbParams::CO2PidSetpoint:
      PIDCO2Update();
      break;
  }
}

// 💧⚙️💭💦♨♨️🌀🛠️🔗🌡️🛠🌡💨✇☢🌫⏱️⏱🕒⏲️⌛⏳↺↻⚠⚠️
inline void WebBuild(sets::Builder &b) {
  debug.tprintln("WebBuild()");

  if (b.build.isAction())
    WebAction(b.build.id, b.build.value);

  if (b.beginGroup("Внутри")) {
    if (b.beginRow("")) {
      b.Label(dbParams::TemperatureIn, "🌡️ Температура", GreenHouse.Sensors.dht22in.Temperature.Text(), GreenHouse.Alerts.Temperature.Color());

      int _tab = db[TemperatureModeIn].toBool();
      if (b.Tabs("📈", &_tab)) {
        db[TemperatureModeIn] = !db[TemperatureModeIn].toBool();
      }
      b.endRow();
    }

    if (!db[TemperatureModeIn].toBool()) {
      b.PlotRunning(dbParams::TemperaturePlotIn, "°C", static_cast<uint16_t>(GreenHouse.getWebUpdateInterval()));
    }

    if (b.beginRow("")) {
      b.Label(dbParams::HumidityIn, "💧 Влажность", GreenHouse.Sensors.dht22in.Humidity.Text(), GreenHouse.Alerts.Humidity.Color());
      int _tab = db[HumidityModeIn].toBool();
      if (b.Tabs("📈", &_tab)) {
        db[HumidityModeIn] = !db[HumidityModeIn].toBool();
      }
      b.endRow();
    }

    if (!db[HumidityModeIn].toBool()) {
      //      b.PlotRunning(dbParams::HumidityPlotIn, "%", db[SensorScanDelay].toInt());

      b.Plot(dbParams::HumidityPlotIn, p.Data(), "%", false);
    }

    if (b.beginRow("")) {
      b.Label(dbParams::CO2In, "💭 CO2", GreenHouse.Sensors.mhz19in.CO2.Text(), GreenHouse.Alerts.CO2.Color());
      int _tab = db[CO2ModeIn].toBool();
      if (b.Tabs("📈", &_tab)) {
        db[CO2ModeIn] = !db[CO2ModeIn].toBool();
      }
      b.endRow();
    }

    if (!db[CO2ModeIn].toBool()) {
      b.PlotRunning(dbParams::CO2PlotIn, "ppm", static_cast<uint16_t>(GreenHouse.getWebUpdateInterval()));
    }

    b.endGroup();
  }

  if (b.beginGroup("Снаружи")) {
    b.Label(dbParams::TemperatureOut, "🌡️ Температура", GreenHouse.Sensors.dht22out.Temperature.Text());
    b.Label(dbParams::HumidityOut, "💧 Влажность", GreenHouse.Sensors.dht22out.Humidity.Text());
    b.Label(dbParams::CO2Out, "💭 CO2", GreenHouse.Sensors.mhz19out.CO2.Text());

    b.endGroup();
  }

  if (b.beginRow("🔌 Устройства", sets::DivType::Block)) {
    b.LED(dbParams::HumidifierLED, "💦", GreenHouse.Devices.Humidifier.State(), (sets::Colors)_AlertColors::LightGray, sets::Colors::Green);
    b.LED(dbParams::FanInnerLED, "🌀", GreenHouse.Devices.FanInner.State(), (sets::Colors)_AlertColors::LightGray, sets::Colors::Green);
    b.LED(dbParams::HeaterLED, "♨️", GreenHouse.Devices.Heater.State(), (sets::Colors)_AlertColors::LightGray, sets::Colors::Green);
    b.LED(dbParams::FanMainLED, "💨", GreenHouse.Devices.FanMain.State(), (sets::Colors)_AlertColors::LightGray, sets::Colors::Green);

    b.endRow();
  }

  if (b.beginGroup("🔗 Контроль")) {
    b.Switch(dbParams::TemperatureControlEnabled, "Температура");

    if (b.beginMenu("Настройки")) {
      b.Slider2(dbParams::TemperatureAlarmThresholdLow, dbParams::TemperatureAlarmThresholdHigh, "Поддерживать", 0, 50, 0.5, " °C");
      b.Slider(dbParams::TemperatureHysteresis, "Отсекать колебания", 0, 10, 0.1, " °C");

      if (b.beginGroup("💨 Вентиляция")) {
        b.Slider(dbParams::TemperatureFanDuration, "Продолжительность", 1, 60, 1, " сек");
        b.Slider(dbParams::TemperatureFanDelay, "Проверка через", 1, 60, 1, " сек");
        b.Slider(dbParams::TemperatureFanEffectiveThreshold, "Порог эффективности", 0, 5, 0.1, " °C");
        b.Slider(dbParams::TemperatureFanNoEffectDelay, "Повторная попытка через", 0, 60, 1, " мин");
        b.endGroup();
      }
      if (b.beginGroup("♨️ Подогрев")) {
        b.Slider(dbParams::TemperatureHeatingDuration, "Продолжительность", 1, 60, 1, " мин");
        b.Slider(dbParams::TemperatureHeatingDelay, "Проверка через", 1, 60, 1, " мин");
        b.Slider(dbParams::TemperatureHeatingEffectiveThreshold, "Порог эффективности", 0, 5, 0.1, " °C");
        b.Slider(dbParams::TemperatureHeatingNoEffectDelay, "Повторная попытка через", 0, 60, 1, " мин");
        b.endGroup();
      }
      if (b.beginGroup("🎛️ PID (тень)")) {
        b.Slider(dbParams::TemperatureSetpoint, "Уставка", 0, 50, 0.5, " °C");
        b.Slider(dbParams::TemperatureKp, "Kp", 0, 20, 0.1, "");
        b.Slider(dbParams::TemperatureKi, "Ki", 0, 1, 0.01, "");
        b.Slider(dbParams::TemperatureKd, "Kd", 0, 20, 0.1, "");
        b.Label(dbParams::TemperaturePidDuty, "Выход нагрев/охлаждение", String("-"), sets::Colors::Gray);
        b.endGroup();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup()) {
    b.Switch(dbParams::HumidityControlEnabled, "Влажность");

    if (b.beginMenu("Настройки")) {
      b.Slider2(dbParams::HumidityAlarmThresholdLow, dbParams::HumidityAlarmThresholdHigh, "Поддерживать", 0, 100, 0.5, " %");
      b.Slider(dbParams::HumidityHysteresis, "Отсекать колебания", 0, 10, 0.1, " %");

      if (b.beginGroup("💨 Вентиляция")) {
        b.Slider(dbParams::HumidityFanDuration, "Продолжительность", 1, 60, 1, " сек");
        b.Slider(dbParams::HumidityFanDelay, "Проверка через", 1, 60, 1, " сек");
        b.Slider(dbParams::HumidityFanEffectiveThreshold, "Порог эффективности", 0, 5, 0.1, " %");
        b.Slider(dbParams::HumidityFanNoEffectDelay, "Повторная попытка через", 0, 60, 1, " мин");
        b.endGroup();
      }
      if (b.beginGroup("💦 Увлажнение")) {
        b.Slider(dbParams::HumidityWettingDuration, "Продолжительность", 1, 60, 1, " сек");
        b.Slider(dbParams::HumidityWettingDelay, "Проверка через", 1, 60, 1, " сек");
        b.Slider(dbParams::HumidityWettingEffectiveThreshold, "Порог эффективности", 0, 5, 0.1, " %");
        b.Slider(dbParams::HumidityWettingNoEffectDelay, "Повторная попытка через", 0, 60, 1, " мин");
        b.endGroup();
      }
      if (b.beginGroup("🎛️ PID (тень)")) {
        b.Slider(dbParams::HumiditySetpoint, "Уставка", 0, 100, 0.5, " %");
        b.Slider(dbParams::HumidityKp, "Kp", 0, 20, 0.1, "");
        b.Slider(dbParams::HumidityKi, "Ki", 0, 1, 0.01, "");
        b.Slider(dbParams::HumidityKd, "Kd", 0, 20, 0.1, "");
        b.Label(dbParams::HumidityPidDuty, "Выход", String("-"), sets::Colors::Gray);
        b.endGroup();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup()) {
    b.Switch(dbParams::CO2ControlEnabled, "CO2");

    if (b.beginMenu("Настройки")) {
      b.Slider(dbParams::CO2AlarmThresholdHigh, "Порог срабатывания", 0, 500, 10, " ppm");
      b.Slider(dbParams::CO2Hysteresis, "Отсекать колебания", 0, 100, 1, " ppm");

      if (b.beginGroup("💨 Вентиляция")) {
        b.Slider(dbParams::CO2FanDuration, "Продолжительность", 1, 60, 1, " сек");
        b.Slider(dbParams::CO2FanDelay, "Проверка через", 1, 60, 1, " сек");
        b.Slider(dbParams::CO2FanEffectiveThreshold, "Порог эффективности", 0, 500, 10, " ppm");
        b.Slider(dbParams::CO2FanNoEffectDelay, "Повторная попытка через", 0, 60, 1, " мин");
        b.endGroup();
      }

      if (b.beginGroup("🎛️ PID (тень)")) {
        b.Slider(dbParams::CO2PidSetpoint, "Уставка разницы", 0, 500, 10, " ppm");
        b.Slider(dbParams::CO2PidKp, "Kp", 0, 20, 0.1, "");
        b.Slider(dbParams::CO2PidKi, "Ki", 0, 1, 0.01, "");
        b.Slider(dbParams::CO2PidKd, "Kd", 0, 20, 0.1, "");
        b.Label(dbParams::CO2PidDuty, "Выход", String("-"), sets::Colors::Gray);
        b.endGroup();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup("🛠️ Настройки")) {
    if (b.beginMenu("📡 MQTT")) {
      if (b.beginGroup("Соединение")) {
        b.Input(dbParams::MqttServer, "Север");
        b.Input(dbParams::MqttPort, "Порт", nullptr, R"(^\d+$)", "Только цифры");
        b.Input(dbParams::MqttUser, "Пользователь");
        b.Pass(dbParams::MqttPassword, "Пароль");
        b.Label("Сотояние", mqtt.Connected() ? "подключен" : "не подключен", mqtt.Connected() ? sets::Colors::Green : sets::Colors::Red);
        if (b.Button(mqtt.Connected() ? "Отсоединить" : "Соединить", mqtt.Connected() ? sets::Colors::Red : sets::Colors::Green)) {
          sett.updater().update(dbParams::MqttReconnectConfirm, mqtt.Connected() ? "Отсоединить?" : "Соединить?");
        }
        b.endGroup();
      }

      if (b.Slider(dbParams::MqttPublishDelay, "Частота публикации", 1, 60, 1, " сек")) {
        mqtt.setPublishDelay(db[MqttPublishDelay].toInt() * 1000ul);
      }

      b.endMenu();
    }

    if (b.beginMenu("📡 Датчики")) {
      b.Slider(dbParams::SensorScanDelay, "⏱️ Частота опроса", 2, 300, 1, " сек");

      if (GreenHouse.Sensors.dht22in.IsValid()) {
        if (b.beginGroup(String(GreenHouse.Sensors.dht22in.SubTypeName()))) {
          b.Number(dbParams::TemperatureInStackSize, "🌡️ Стек", nullptr, 1, 100);
          b.Number(dbParams::HumidityInStackSize, "💧 Стек", nullptr, 1, 100);
          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.dht22out.IsValid()) {
        if (b.beginGroup(String(GreenHouse.Sensors.dht22out.SubTypeName()))) {
          b.Number(dbParams::TemperatureOutStackSize, "🌡️ Стек", nullptr, 1, 100);
          b.Number(dbParams::HumidityOutStackSize, "💧 Стек", nullptr, 1, 100);
          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.mhz19in.IsValid()) {
        if (b.beginGroup(String(GreenHouse.Sensors.mhz19in.SubTypeName()))) {
          localParams.mhz19in_Range           = GreenHouse.Sensors.mhz19in.Range == 2000 ? 0 : 1;
          localParams.mhz19in_AutoCalibration = GreenHouse.Sensors.mhz19in.AutoCalibration;

          b.Number(dbParams::CO2InStackSize, "💭 Стек", nullptr, 1, 100);

          if (b.Select("Разрешение", "2000;5000", &localParams.mhz19in_Range)) {
            GreenHouse.Sensors.mhz19in.setRange(localParams.mhz19in_Range == 0 ? 2000 : 5000);
          }

          if (b.Switch("Автокалибровка", &localParams.mhz19in_AutoCalibration)) {
            GreenHouse.Sensors.mhz19in.setAutoCalibration(localParams.mhz19in_AutoCalibration);
          }

          if (b.Button("Откалибровать на 400 ppm")) {
            sett.updater().update(dbParams::CO2InCalibrationConfirm, "Уверены?");
          }

          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.mhz19out.IsValid()) {
        if (b.beginGroup(String(GreenHouse.Sensors.mhz19out.SubTypeName()))) {
          localParams.mhz19out_Range           = GreenHouse.Sensors.mhz19out.Range == 2000 ? 0 : 1;
          localParams.mhz19out_AutoCalibration = GreenHouse.Sensors.mhz19out.AutoCalibration;

          b.Number(dbParams::CO2OutStackSize, "💭 Стек", nullptr, 1, 100);

          if (b.Select("Разрешение", "2000;5000", &localParams.mhz19out_Range)) {
            GreenHouse.Sensors.mhz19out.setRange(localParams.mhz19out_Range == 0 ? 2000 : 5000);
          }

          if (b.Switch("Автокалибровка", &localParams.mhz19out_AutoCalibration)) {
            GreenHouse.Sensors.mhz19out.setAutoCalibration(localParams.mhz19out_AutoCalibration);
          }

          if (b.Button("Откалибровать на 400 ppm")) {
            sett.updater().update(dbParams::CO2OutCalibrationConfirm, "Уверены?");
          }

          b.endGroup();
        }
      }

      bool _begin = false;

      if (GreenHouse.Sensors.dht22out.IsValid() && GreenHouse.Sensors.dht22in.IsValid()) {
        if (b.beginGroup("Синхронизация внешних датчиков")) {
          _begin = true;

          if (GreenHouse.Sync.SubTypeIs(_SubType::Temperature)) {
            b.Label(dbParams::TemperatureSyncPercent, "Синхронизация 🌡️", GreenHouse.Sync.MillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::TemperatureSyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::TemperatureOffset, "Компенсация 🌡️");

            if (b.beginRow()) {
              if (b.Button("Синхронизировать")) {
                sett.updater().update(dbParams::TemperatureSyncConfirm, "Уверены?");
              }
              if (b.Button("Сбросить")) {
                sett.updater().update(dbParams::TemperatureClearOffsetConfirm, "Уверены?");
              }
              b.endRow();
            }
          }
          b.endGroup();
        }

        if (b.beginGroup()) {
          if (GreenHouse.Sync.SubTypeIs(_SubType::Humidity)) {
            b.Label(dbParams::HumiditySyncPercent, "Синхронизация 💧", GreenHouse.Sync.MillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::HumiditySyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::HumidityOffset, "Компенсация 💧");

            if (b.beginRow()) {
              if (b.Button("Синхронизировать")) {
                sett.updater().update(dbParams::HumiditySyncConfirm, "Уверены?");
              }
              if (b.Button("Сбросить")) {
                sett.updater().update(dbParams::HumidityClearOffsetConfirm, "Уверены?");
              }
              b.endRow();
            }
          }
          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.mhz19out.IsValid() && GreenHouse.Sensors.mhz19in.IsValid() &&
          !GreenHouse.Sensors.mhz19out.IsHeating() && !GreenHouse.Sensors.mhz19in.IsHeating()) {
        if (b.beginGroup((!_begin) ? "Синхронизация внешних датчиков" : "")) {
          if (GreenHouse.Sync.SubTypeIs(_SubType::CO2)) {
            b.Label(dbParams::CO2SyncPercent, "Синхронизация 💭", GreenHouse.Sync.MillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::CO2SyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::CO2Offset, "Компенсация 💭");

            if (b.beginRow()) {
              if (b.Button("Синхронизировать")) {
                sett.updater().update(dbParams::CO2SyncConfirm, "Уверены?");
              }
              if (b.Button("Сбросить")) {
                sett.updater().update(dbParams::CO2ClearOffsetConfirm, "Уверены?");
              }
              b.endRow();
            }
          }
          b.endGroup();
        }
      }

      b.endMenu();
    }

    if (b.beginMenu("📊 Информация")) {
      if (b.beginGroup("Лог")) {
        b.Log(H(log), logger);

        b.endGroup();
      }

      b.Select(dbParams::DebugMode, "Отладка", "Off;Serial;Logger;All");

      if (b.Button("Очистить")) {
        logger.clear();
        logger.println();

        sett.updater().update(H(log), logger);
      }

#ifdef DEBUG_ENABLE
      if (b.Button("controller dump")) {
        HC.dump();
      }

      if (b.Button("Изменения показаний")) {
        std::vector<std::reference_wrapper<_ReadingsStat>> r = {
            GreenHouse.Sensors.dht22in.Temperature.Stat,
            GreenHouse.Sensors.dht22in.Humidity.Stat,
            GreenHouse.Sensors.mhz19in.CO2.Stat,
            GreenHouse.Sensors.dht22out.Temperature.Stat,
            GreenHouse.Sensors.dht22out.Humidity.Stat,
            GreenHouse.Sensors.mhz19out.CO2.Stat};

        logger.clear();

        for (auto &stat_ref : r) {
          _ReadingsStat &stat = stat_ref.get();

          if (stat.IsEnabled()) {
            logger.printf("=== %s.%s ===\n", stat.Owner().Name(), stat.Name());
            logger.println("~");
            logger.printf("Stat.MillisFrom: %lu\n", stat.MillisFrom);
            logger.printf("Stat.MillisTo: %lu\n", stat.MillisTo);
            logger.printf("Stat.ValueFrom: %.2f\n", stat.ValueFrom);
            logger.printf("Stat.ValueTo: %.2f\n", stat.ValueTo);
            logger.printf("Stat.Min: %.2f\n", stat.Min);
            logger.printf("Stat.Max: %.2f\n", stat.Max);
            logger.printf("Stat.MinMillis: %lu\n", stat.MinMillis);
            logger.printf("Stat.MaxMillis: %lu\n", stat.MaxMillis);
            logger.printf("Stat.Duration: %lu\n", stat.Duration);
            logger.printf("Stat.Diff: %.2f\n", stat.Diff);
            logger.printf("Stat.DiffHysteresis: %.2f\n", stat.DiffHysteresis);
            logger.printf("Stat.TotalDiff: %.2f\n", stat.TotalDiff);
            logger.printf("Stat.TotalDiffPercent: %.2f\n", stat.TotalDiffPercent);
            logger.printf("Stat.Sum: %.2f\n", stat.Sum);
            logger.printf("Stat.Avg: %.2f\n", stat.Avg);
            logger.printf("Stat.Count: %llu\n", static_cast<unsigned long long>(stat.Count));
            logger.printf("Stat.IsUp: %s\n", stat.IsUp() ? "true" : "false");
            logger.printf("Stat.IsDown: %s\n", stat.IsDown() ? "true" : "false");
            logger.printf("Stat.IsUpBy: %.2f\n", stat.IsUpBy());
            logger.printf("Stat.IsDownBy: %.2f\n", stat.IsDownBy());
            logger.printf("Stat.AbsoluteUp: %.2f\n", stat.AbsoluteUp());
            logger.printf("Stat.AbsoluteDown: %.2f\n", stat.AbsoluteDown());
            logger.printf("Stat.IsValid: %s\n", stat.IsValid ? "true" : "false");
            logger.println("~");
            logger.printf("Events.Change.Old: %.2f\n", stat.Events.Change.Old);
            logger.printf("Events.Change.New: %.2f\n", stat.Events.Change.New);
            logger.printf("Events.Change.Diff: %.2f\n", stat.Events.Change.Diff);
            logger.printf("Events.Change.Millis: %lu\n", stat.Events.Change.Millis);
            logger.printf("Events.Change.IsValid: %s\n", stat.Events.Change.IsValid ? "true" : "false");
            logger.println("~");
            logger.printf("Events.Revert.Old: %.2f\n", stat.Events.Revert.Old);
            logger.printf("Events.Revert.New: %.2f\n", stat.Events.Revert.New);
            logger.printf("Events.Revert.Diff: %.2f\n", stat.Events.Revert.Diff);
            logger.printf("Events.Revert.Millis: %lu\n", stat.Events.Revert.Millis);
            logger.printf("Events.Revert.IsValid: %s\n", stat.Events.Revert.IsValid ? "true" : "false");
            logger.println("~");
            logger.printf("Events.Increase.Old: %.2f\n", stat.Events.Increase.Old);
            logger.printf("Events.Increase.New: %.2f\n", stat.Events.Increase.New);
            logger.printf("Events.Increase.Diff: %.2f\n", stat.Events.Increase.Diff);
            logger.printf("Events.Increase.Millis: %lu\n", stat.Events.Increase.Millis);
            logger.printf("Events.Increase.IsValid: %s\n", stat.Events.Increase.IsValid ? "true" : "false");
            logger.println("~");
            logger.printf("Events.Decrease.Old: %.2f\n", stat.Events.Decrease.Old);
            logger.printf("Events.Decrease.New: %.2f\n", stat.Events.Decrease.New);
            logger.printf("Events.Decrease.Diff: %.2f\n", stat.Events.Decrease.Diff);
            logger.printf("Events.Decrease.Millis: %lu\n", stat.Events.Decrease.Millis);
            logger.printf("Events.Decrease.IsValid: %s\n", stat.Events.Decrease.IsValid ? "true" : "false");
          }
        }

        sett.updater().update(H(log), logger);
      }

      if (b.Button("История, стек")) {
        logger.clear();

        logger.println("=== Stack Size ===");

        logger.printf("dht22in.Temperature.StackSize = %u\n", GreenHouse.Sensors.dht22in.Temperature.StackSize());
        logger.printf("dht22in.Humidity.StackSize = %u\n", GreenHouse.Sensors.dht22in.Humidity.StackSize());
        logger.printf("mhz19in.CO2.StackSize = %u\n", GreenHouse.Sensors.mhz19in.CO2.StackSize());
        logger.printf("dht22out.Temperature.StackSize = %u\n", GreenHouse.Sensors.dht22out.Temperature.StackSize());
        logger.printf("dht22out.Humidity.StackSize = %u\n", GreenHouse.Sensors.dht22out.Humidity.StackSize());
        logger.printf("mhz19out.CO2.StackSize = %u\n", GreenHouse.Sensors.mhz19out.CO2.StackSize());

        logger.println("~");
        logger.println("=== Readings Offset ===");

        logger.printf("dht22in.Temperature.Offset = %.4f\n", GreenHouse.Sensors.dht22in.TemperatureOffset());
        logger.printf("dht22in.Humidity.Offset = %.4f\n", GreenHouse.Sensors.dht22in.HumidityOffset());
        logger.printf("mhz19in.CO2.Offset = %.4f\n", GreenHouse.Sensors.mhz19in.CO2Offset());
        logger.printf("dht22out.Temperature.Offset = %.4f\n", GreenHouse.Sensors.dht22out.TemperatureOffset());
        logger.printf("dht22out.Humidity.Offset = %.4f\n", GreenHouse.Sensors.dht22out.HumidityOffset());
        logger.printf("mhz19out.CO2.Offset = %.4f\n", GreenHouse.Sensors.mhz19out.CO2Offset());

        logger.println("~");
        logger.println("=== Readings Hysteresis ===");

        logger.printf("dht22in.Temperature.Hysteresis = %.4f\n", GreenHouse.Sensors.dht22in.TemperatureHysteresis());
        logger.printf("dht22in.Humidity.Hysteresis = %.4f\n", GreenHouse.Sensors.dht22in.HumidityHysteresis());
        logger.printf("mhz19in.CO2.Hysteresis = %.4f\n", GreenHouse.Sensors.mhz19in.CO2Hysteresis());
        logger.printf("dht22out.Temperature.Hysteresis = %.4f\n", GreenHouse.Sensors.dht22out.TemperatureHysteresis());
        logger.printf("dht22out.Humidity.Hysteresis = %.4f\n", GreenHouse.Sensors.dht22out.HumidityHysteresis());
        logger.printf("mhz19out.CO2.Hysteresis = %.4f\n", GreenHouse.Sensors.mhz19out.CO2Hysteresis());

        logger.println("~");
        logger.println("=== Readings Accuracy ===");

        logger.printf("dht22in.Temperature.Accuracy = %.4f\n", GreenHouse.Sensors.dht22in.TemperatureAccuracy());
        logger.printf("dht22in.Humidity.Accuracy = %.4f\n", GreenHouse.Sensors.dht22in.HumidityAccuracy());
        logger.printf("mhz19in.CO2.Accuracy = %.4f\n", GreenHouse.Sensors.mhz19in.CO2Accuracy());
        logger.printf("dht22out.Temperature.Accuracy = %.4f\n", GreenHouse.Sensors.dht22out.TemperatureAccuracy());
        logger.printf("dht22out.Humidity.Accuracy = %.4f\n", GreenHouse.Sensors.dht22out.HumidityAccuracy());
        logger.printf("mhz19out.CO2.Accuracy = %.4f\n", GreenHouse.Sensors.mhz19out.CO2Accuracy());

        sett.updater().update(H(log), logger);
      }

#endif

      if (b.Button("Состояние памяти")) {
        size_t total_heap     = ESP.getHeapSize();
        size_t free_heap      = ESP.getFreeHeap();
        size_t used_heap      = total_heap - free_heap;
        size_t min_free_heap  = ESP.getMinFreeHeap();
        size_t max_alloc_heap = ESP.getMaxAllocHeap();

        size_t free_dram         = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_8bit         = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t free_dma          = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t free_default      = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t largest_free_dram = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

        multi_heap_info_t heap_info;
        heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);

        bool heap_ok = heap_caps_check_integrity(MALLOC_CAP_DEFAULT, true);

        logger.clear();

        logger.println("=== Memory Summary ===");
        logger.printf("Total Heap: %u bytes\n", total_heap);
        logger.printf("Used Heap: %u bytes\n", used_heap);
        logger.printf("Free Heap: %u bytes\n", free_heap);

        if (psramFound()) {
          size_t total_psram        = ESP.getPsramSize();
          size_t free_psram         = ESP.getFreePsram();
          size_t used_psram         = total_psram - free_psram;
          size_t largest_free_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

          logger.println("~");
          logger.printf("Total PSRAM: %u bytes\n", total_psram);
          logger.printf("Used PSRAM: %u bytes\n", used_psram);
          logger.printf("Free PSRAM: %u bytes\n", free_psram);
          logger.printf("Largest PSRAM Block: %u bytes\n", largest_free_psram);
        }

        logger.println("~");
        logger.println("=== Memory Details ===");
        logger.printf("DRAM Free: %u bytes\n", free_dram);
        logger.printf("8-bit Access Free: %u bytes\n", free_8bit);
        logger.printf("DMA Capable Free: %u bytes\n", free_dma);
        logger.printf("Largest DRAM Block: %u bytes\n", largest_free_dram);

        logger.println("~");
        logger.println("=== Heap Status ===");
        logger.printf("Free Blocks: %u\n", heap_info.free_blocks);
        logger.printf("Allocated Blocks: %u\n", heap_info.allocated_blocks);
        logger.printf("Largest Free Block: %u bytes\n", heap_info.largest_free_block);
        logger.printf("Heap Integrity: %s\n", heap_ok ? "OK" : "Corrupted");

        logger.println("~");
        logger.println("=== System Info ===");
        logger.printf("Min Free Heap: %u bytes\n", min_free_heap);
        logger.printf("Max Alloc Heap Block: %u bytes\n", max_alloc_heap);
        logger.printf("Stack Watermark: %u\n", uxTaskGetStackHighWaterMark(nullptr));

        sett.updater().update(H(log), logger);
      }

      if (b.Button("Статус Wi-Fi")) {
        logger.clear();

        if (WiFi.status() == WL_CONNECTED) {
          logger.println("Wi-Fi Status: Connected");
          logger.print("SSID: ");
          logger.println(WiFi.SSID());
          logger.print("BSSID: ");
          logger.println(WiFi.BSSIDstr());
          //          logger.print("RSSI: ");
          //          logger.print(WiFi.RSSI());
          //          logger.println(" dBm");

          int rssi = WiFi.RSSI();

          logger.print("Signal: ");
          logger.print(rssi);
          logger.println(" dBm");

          logger.print("Quality: ");
          if (rssi >= -50) {
            logger.println("Excellent");
          } else if (rssi >= -60) {
            logger.println("Good");
          } else if (rssi >= -70) {
            logger.println("Normal" /*"Fair"*/);
          } else if (rssi >= -80) {
            logger.println("Poor");
          } else {
            logger.println("Very poor");
          }

          wifi_ap_record_t apInfo;
          if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
            // logger.print("SSID: ");
            // logger.println((char *)apInfo.ssid);
            // logger.print("BSSID: ");
            // logger.println(WiFi.BSSIDstr());
            logger.print("Channel: ");
            logger.println(apInfo.primary);
            logger.print("Frequency: ");
            if (apInfo.primary <= 14) {
              logger.println("2.4 GHz");
            } else {
              logger.println("5 GHz");
            }
          } else {
            logger.println("Channel: Error");
            logger.println("Frequency: Error");
          }

          logger.print("IP Address: ");
          logger.println(WiFi.localIP());
          logger.print("Subnet Mask: ");
          logger.println(WiFi.subnetMask());
          logger.print("Gateway: ");
          logger.println(WiFi.gatewayIP());
          logger.print("DNS Server: ");
          logger.println(WiFi.dnsIP());
          logger.print("MAC Address: ");
          logger.println(WiFi.macAddress());
        } else {
          logger.println("Wi-Fi Status: Disconnected");
        }

        sett.updater().update(H(log), logger);
      }

      if (b.Button("Перезагрузка", sets::Colors::Red)) {
        sett.updater().update(dbParams::RestartConfirm, "Перезагрузить контроллер?");
      }

      b.endMenu();
    }

    b.endGroup();
  }

  if (b.Confirm(dbParams::CO2InCalibrationConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      if (GreenHouse.Sensors.mhz19in.IsValid()) {
        GreenHouse.Sensors.mhz19in.Calibrate400();
      }
    }
  }

  if (b.Confirm(dbParams::CO2OutCalibrationConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      if (GreenHouse.Sensors.mhz19out.IsValid()) {
        GreenHouse.Sensors.mhz19out.Calibrate400();
      }
    }
  }

  if (b.Confirm(dbParams::RestartConfirm, "Перезагрузить контроллер?")) {
    if (b.build.value.toBool()) {
      ESP.restart();
    }
  }

  if (b.Confirm(dbParams::TemperatureSyncConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Start(&GreenHouse.Sensors.dht22in.Temperature,
                            &GreenHouse.Sensors.dht22out.Temperature);
    }
  }

  if (b.Confirm(dbParams::HumiditySyncConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Start(&GreenHouse.Sensors.dht22in.Humidity,
                            &GreenHouse.Sensors.dht22out.Humidity);
    }
  }

  if (b.Confirm(dbParams::CO2SyncConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Start(&GreenHouse.Sensors.mhz19in.CO2,
                            &GreenHouse.Sensors.mhz19out.CO2);
    }
  }

  if (b.Confirm(dbParams::TemperatureSyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Cancel();
    }
  }

  if (b.Confirm(dbParams::HumiditySyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Cancel();
    }
  }

  if (b.Confirm(dbParams::CO2SyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sync.Cancel();
    }
  }

  if (b.Confirm(dbParams::TemperatureClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.dht22out.setTemperatureOffset(0);
      db[TemperatureOffset] = 0;
    }
  }

  if (b.Confirm(dbParams::HumidityClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.dht22out.setHumidityOffset(0);
      db[HumidityOffset] = 0;
    }
  }

  if (b.Confirm(dbParams::CO2ClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.mhz19out.setCO2Offset(0);
      db[CO2Offset] = 0;
    }
  }

  if (b.Confirm(dbParams::MqttReconnectConfirm, mqtt.Connected() ? "Отсоединить?" : "Соединить?")) {
    if (b.build.value.toBool()) {
      if (mqtt.Connected())
        mqtt.Disconnect();
      else
        mqtt.Connect(db[MqttServer].toString().c_str(),
                     db[MqttPort].toInt(),
                     db[MqttUser].toString().c_str(),
                     db[MqttPassword].toString().c_str());

      b.reload();
    }
  }
}

inline void AlertStateChanged(_Alert &Alert, _AlertState StateFrom, _AlertState StateTo) {
  // debug.tprintf("%s.AlertStateChanged: %s > %s\n",
  //               Type.Name(),
  //               AlertStateName(StateFrom),
  //               AlertStateName(StateTo));
}

inline void DeviceActiveChanged(_Device &Device, bool IsActive) {
  //  debug.tprintf("%s.%s\n", Device.Name(), IsActive ? "Start()" : "Stop()");
}

inline void DeviceStateChanged(_Device &Device, bool State) {
  //  debug.tprintf("%s.%s\n", Device.Name(), State ? "TurnOn()" : "TurnOff()");
}

inline void StatDirectionChange(_ReadingsStat &Stat, float Old, float New, bool Increase) {
}

inline void StatValueChange(_ReadingsStat &Stat, float Old, float New, bool Increase, bool DirectionChanged) {
}

inline void StatValueAdd(_ReadingsStat &Stat, float Value, ulong LastChangeMillisElapsed) {
}

// inline void StatEnabledChange(_ReadingsStat &Stat, bool IsEnabled) {
// }

inline void mqttPublishAll() {
  mqtt.Publish("FanMain/IsActive", GreenHouse.Devices.FanMain.State());
  mqtt.Publish("FanInner/IsActive", GreenHouse.Devices.FanInner.State());
  mqtt.Publish("Humidifier/IsActive", GreenHouse.Devices.Humidifier.State());
  mqtt.Publish("Heater/IsActive", GreenHouse.Devices.Heater.State());

  mqtt.Publish("Temperature/ValueIn", GreenHouse.Sensors.dht22in.Temperature.Value());
  mqtt.Publish("Temperature/ValueOut", GreenHouse.Sensors.dht22out.Temperature.Value());
  mqtt.Publish("Temperature/ControlNotEffective", GreenHouse.Alerts.Temperature.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}));
  mqtt.Publish("Temperature/AlertState", static_cast<int>(GreenHouse.Alerts.Temperature.State()));

  mqtt.Publish("Humidity/ValueIn", GreenHouse.Sensors.dht22in.Humidity.Value());
  mqtt.Publish("Humidity/ValueOut", GreenHouse.Sensors.dht22out.Humidity.Value());
  mqtt.Publish("Humidity/ControlNotEffective", GreenHouse.Alerts.Humidity.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}));
  mqtt.Publish("Humidity/AlertState", static_cast<int>(GreenHouse.Alerts.Humidity.State()));

  mqtt.Publish("CO2/ValueIn", GreenHouse.Sensors.mhz19in.CO2.Value());
  mqtt.Publish("CO2/ValueOut", GreenHouse.Sensors.mhz19out.CO2.Value());
  mqtt.Publish("CO2/ControlNotEffective", GreenHouse.Alerts.CO2.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}));
  mqtt.Publish("CO2/AlertState", static_cast<int>(GreenHouse.Alerts.CO2.State()));
}

inline void mqttSubscribeAll() {
  //
}

void mqttCallBack(const char *topic, const char *value, const uint length, const dbParams dbParam, const bool IsWrited) {
  if ((dbParam != dbParams::Undefined) && IsWrited)
    WebAction(dbParam, value);
}

inline void ReadingsBeforeSync(_Sync &Sync) {
  switch (Sync.SubType()) {
    case _SubType::Temperature:
      db[TemperatureOffset] = 0;
      break;

    case _SubType::Humidity:
      db[HumidityOffset] = 0;
      break;

    case _SubType::CO2:
      db[CO2Offset] = 0;
      break;
  }
}

inline void ReadingsAfterSync(_Sync &Sync) {
  switch (Sync.SubType()) {
    case _SubType::Temperature:
      db[TemperatureOffset] = GreenHouse.Sensors.dht22out.TemperatureOffset();
      break;

    case _SubType::Humidity:
      db[HumidityOffset] = GreenHouse.Sensors.dht22out.HumidityOffset();
      break;

    case _SubType::CO2:
      db[CO2Offset] = GreenHouse.Sensors.mhz19out.CO2Offset();
      break;
  }
}

inline void GetTemperatureInPrefix(char *Prefix) {
  if (GreenHouse.Alerts.Temperature.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}))
    strcpy(Prefix, "⚠");
  else if (GreenHouse.Alerts.Temperature.StateIs({_AlertState::LowSuspended, _AlertState::HighSuspended}))
    strcpy(Prefix, "װ");
  else
    Prefix[0] = '\0';
}

inline void GetHumidityInPrefix(char *Prefix) {
  if (GreenHouse.Alerts.Humidity.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}))
    strcpy(Prefix, "⚠");
  else if (GreenHouse.Alerts.Humidity.StateIs({_AlertState::LowSuspended, _AlertState::HighSuspended}))
    strcpy(Prefix, "װ");
  else
    Prefix[0] = '\0';
}

inline void GetCO2InPrefix(char *Prefix) {
  if (GreenHouse.Alerts.CO2.StateIs({_AlertState::LowNoEffect, _AlertState::HighNoEffect}))
    strcpy(Prefix, "⚠");
  else if (GreenHouse.Alerts.CO2.StateIs({_AlertState::LowSuspended, _AlertState::HighSuspended}))
    strcpy(Prefix, "װ");
  else
    Prefix[0] = '\0';
}

inline void GetTemperatureOutPrefix(char *Prefix) {
  if (GreenHouse.Sync.SubTypeIs(_SubType::Temperature))
    strcpy(Prefix, "⇄");
  else
    Prefix[0] = '\0';
}

inline void GetHumidityOutPrefix(char *Prefix) {
  if (GreenHouse.Sync.SubTypeIs(_SubType::Humidity))
    strcpy(Prefix, "⇄");
  else
    Prefix[0] = '\0';
}

inline void GetCO2OutPrefix(char *Prefix) {
  if (GreenHouse.Sync.SubTypeIs(_SubType::CO2))
    strcpy(Prefix, "⇄");
  else
    Prefix[0] = '\0';
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  setStampZone(3);

  _FS_Initialized = LittleFS.begin();

  if (!_FS_Initialized) {
    LittleFS.end();

    _FS_Initialized = LittleFS.begin(true);
  }

  WiFi.setSleep(false);
  // WiFi.setTxPower(WIFI_POWER_11dBm);

  WiFiConnector.onConnect([]() {
    debug.tprint("Local IP: ");
    debug.println(WiFi.localIP());
  });

  WiFiConnector.onError([]() {
    debug.tprintln("WiFi error");
  });

  WiFiConnector.connect(WIFI_SSID, WIFI_PASS);

  sett.begin();
  sett.onBuild(WebBuild);
  //  sett.onUpdate(WebUpdate);
  sett.onFocusChange([]() {
    // debug.tprint("Focus: ");
    // debug.println(sett.focused());
  });

  // sett.config.requestTout = 3000;
  // sett.config.sliderTout = 500;
  // sett.config.updateTout = 1000;
  // sett.config.theme = sets::Colors::Black;

  db.begin();

  db.init(dbParams::SensorScanDelay, (uint)2);

  db.init(dbParams::TemperatureControlEnabled, (bool)0);
  db.init(dbParams::TemperatureAlarmThresholdLow, (float)15);
  db.init(dbParams::TemperatureAlarmThresholdHigh, (float)19);
  db.init(dbParams::TemperatureFanDuration, (uint)5);
  db.init(dbParams::TemperatureFanDelay, (uint)20);
  db.init(dbParams::TemperatureHeatingDuration, (uint)5);
  db.init(dbParams::TemperatureHeatingDelay, (uint)10);

  db.init(dbParams::HumidityControlEnabled, (bool)0);
  db.init(dbParams::HumidityAlarmThresholdLow, (float)85);
  db.init(dbParams::HumidityAlarmThresholdHigh, (float)95);
  db.init(dbParams::HumidityFanDuration, (uint)5);
  db.init(dbParams::HumidityFanDelay, (uint)20);
  db.init(dbParams::HumidityWettingDuration, (uint)10);
  db.init(dbParams::HumidityWettingDelay, (uint)30);

  db.init(dbParams::HumiditySetpoint, (float)90);
  db.init(dbParams::HumidityKp, (float)2);
  db.init(dbParams::HumidityKi, (float)0.01);
  db.init(dbParams::HumidityKd, (float)0);
  db.init(dbParams::HumidityPidDuty, (Text) "-");

  db.init(dbParams::TemperatureSetpoint, (float)17);
  db.init(dbParams::TemperatureKp, (float)2);
  db.init(dbParams::TemperatureKi, (float)0.005);
  db.init(dbParams::TemperatureKd, (float)0);
  db.init(dbParams::TemperaturePidDuty, (Text) "-");

  db.init(dbParams::CO2PidSetpoint, (float)100);
  db.init(dbParams::CO2PidKp, (float)0);
  db.init(dbParams::CO2PidKi, (float)0.05);
  db.init(dbParams::CO2PidKd, (float)0);
  db.init(dbParams::CO2PidDuty, (Text) "-");

  db.init(dbParams::CO2ControlEnabled, (bool)0);
  db.init(dbParams::CO2AlarmThresholdHigh, (float)1500);
  db.init(dbParams::CO2FanDuration, (uint)5);
  db.init(dbParams::CO2FanDelay, (uint)20);

  db.init(dbParams::TemperatureFanEffectiveThreshold, (float)1);
  db.init(dbParams::TemperatureHeatingEffectiveThreshold, (float)1);
  db.init(dbParams::HumidityFanEffectiveThreshold, (float)1);
  db.init(dbParams::HumidityWettingEffectiveThreshold, (float)1);
  db.init(dbParams::CO2FanEffectiveThreshold, (float)50);

  db.init(dbParams::TemperatureFanNoEffectDelay, (uint)10);
  db.init(dbParams::TemperatureHeatingNoEffectDelay, (uint)10);
  db.init(dbParams::HumidityFanNoEffectDelay, (uint)10);
  db.init(dbParams::HumidityWettingNoEffectDelay, (uint)10);
  db.init(dbParams::CO2FanNoEffectDelay, (uint)10);

  db.init(dbParams::TemperatureInStackSize, (size_t)3);
  db.init(dbParams::HumidityInStackSize, (size_t)3);
  db.init(dbParams::CO2InStackSize, (size_t)3);

  db.init(dbParams::TemperatureOutStackSize, (size_t)3);
  db.init(dbParams::HumidityOutStackSize, (size_t)3);
  db.init(dbParams::CO2OutStackSize, (size_t)3);

  db.init(dbParams::TemperatureOffset, (float)0);
  db.init(dbParams::HumidityOffset, (float)0);
  db.init(dbParams::CO2Offset, (float)0);

  db.init(dbParams::TemperatureHysteresis, (float)0.5);
  db.init(dbParams::HumidityHysteresis, (float)0.5);
  db.init(dbParams::CO2Hysteresis, (float)50);

  db.init(dbParams::TemperatureModeIn, (byte)1);
  db.init(dbParams::HumidityModeIn, (byte)1);
  db.init(dbParams::CO2ModeIn, (byte)1);

  db.init(dbParams::TemperatureModeOut, (byte)1);
  db.init(dbParams::HumidityModeOut, (byte)1);
  db.init(dbParams::CO2ModeOut, (byte)1);

  db.init(dbParams::MqttServer, (Text) "srv2.clusterfly.ru");
  db.init(dbParams::MqttPort, (uint)9991);
  db.init(dbParams::MqttUser, (Text) "****");
  db.init(dbParams::MqttPassword, (Text) "****");

  db.init(dbParams::MqttPublishDelay, (uint)5);

  db.init(dbParams::DebugMode, (uint8_t)0);

  debug.setModeInt(db[dbParams::DebugMode].toInt());

  GreenHouse.setSensorUpdateInterval(db[SensorScanDelay].toInt() * 1000ul);

  GreenHouse.Sensors.dht22in.setTemperatureStackSize(db[TemperatureInStackSize].toInt());
  GreenHouse.Sensors.dht22in.setHumidityStackSize(db[HumidityInStackSize].toInt());
  GreenHouse.Sensors.mhz19in.setCO2StackSize(db[CO2InStackSize].toInt());

  GreenHouse.Sensors.dht22out.setTemperatureStackSize(db[TemperatureOutStackSize].toInt());
  GreenHouse.Sensors.dht22out.setHumidityStackSize(db[HumidityOutStackSize].toInt());
  GreenHouse.Sensors.mhz19out.setCO2StackSize(db[CO2OutStackSize].toInt());

  GreenHouse.Sensors.dht22out.setTemperatureOffset(db[TemperatureOffset].toFloat());
  GreenHouse.Sensors.dht22out.setHumidityOffset(db[HumidityOffset].toFloat());
  GreenHouse.Sensors.mhz19out.setCO2Offset(db[CO2Offset].toFloat());

  GreenHouse.Sensors.dht22in.setTemperatureHysteresis(db[TemperatureHysteresis].toFloat());
  GreenHouse.Sensors.dht22in.setHumidityHysteresis(db[HumidityHysteresis].toFloat());
  GreenHouse.Sensors.mhz19in.setCO2Hysteresis(db[CO2Hysteresis].toFloat());

  PIDHumidity.setWindow(PID_WINDOW_DEFAULT);
  PIDHumidity.setSampleTime(PID_SAMPLE_DEFAULT);
  PIDHumidity.setDutyMin(5000ul, 5000ul);
  PIDHumidityUpdate();

  PIDTemperature.setOutputLimits(-100, 100);
  PIDTemperature.setWindow(PID_WINDOW_DEFAULT);
  PIDTemperature.setSampleTime(PID_SAMPLE_DEFAULT);
  PIDTemperature.setDutyMin(30000ul, 30000ul);
  PIDTemperatureUpdate();

  PIDCO2.setWindow(PID_WINDOW_DEFAULT);
  PIDCO2.setSampleTime(PID_SAMPLE_DEFAULT);
  PIDCO2.setDutyMin(5000ul, 5000ul);
  PIDCO2Update();

  GreenHouse.Sensors.dht22in.Temperature.OnGetPrefix(GetTemperatureInPrefix);
  GreenHouse.Sensors.dht22in.Humidity.OnGetPrefix(GetHumidityInPrefix);
  GreenHouse.Sensors.mhz19in.CO2.OnGetPrefix(GetCO2InPrefix);

  GreenHouse.Sensors.dht22out.Temperature.OnGetPrefix(GetTemperatureOutPrefix);
  GreenHouse.Sensors.dht22out.Humidity.OnGetPrefix(GetHumidityOutPrefix);
  GreenHouse.Sensors.mhz19out.CO2.OnGetPrefix(GetCO2OutPrefix);

  GreenHouse.Alerts.OnAlertStateChanged(AlertStateChanged);
  GreenHouse.Devices.OnDeviceActiveChanged(DeviceActiveChanged);
  GreenHouse.Devices.OnDeviceStateChanged(DeviceStateChanged);
  GreenHouse.Sensors.OnStatDirectionChange(StatDirectionChange);
  GreenHouse.Sensors.OnStatValueChange(StatValueChange);
  GreenHouse.Sensors.OnStatValueAdd(StatValueAdd);
  // GreenHouse.Sensors.OnStatEnabledChange(StatEnabledChange);

  GreenHouse.OnBeforeSync(ReadingsBeforeSync);
  GreenHouse.OnAfterSync(ReadingsAfterSync);
  GreenHouse.OnCancelSync(ReadingsAfterSync);

  GreenHouse.OnAlertsUpdate(AlertsUpdate);
  GreenHouse.OnDevicesUpdate(DevicesUpdate);
  GreenHouse.OnWebUpdate(WebUpdate);

  mqtt.setPublishDelay(db[MqttPublishDelay].toInt() * 1000ul);

  mqtt.Init(db[MqttServer].toString().c_str(),
            db[MqttPort].toInt(),
            db[MqttUser].toString().c_str(),
            db[MqttPassword].toString().c_str());

  GreenHouse.OnMqttPublishAll(mqttPublishAll);
  GreenHouse.OnMqttSubscribeAll(mqttSubscribeAll);
  GreenHouse.OnMqttCallback(mqttCallBack);

  Params[dbParams::DHT22InExists]  = false;
  Params[dbParams::DHT22OutExists] = false;
  Params[dbParams::MHZ19InExists]  = false;
  Params[dbParams::MHZ19OutExists] = false;

  Params[dbParams::MHZ19InHeating]  = false;
  Params[dbParams::MHZ19OutHeating] = false;

  Params[dbParams::TemperatureIsSync] = false;
  Params[dbParams::HumidityIsSync]    = false;
  Params[dbParams::CO2IsSync]         = false;

  Params[dbParams::TemperatureModeIn] = db[TemperatureModeIn];
  Params[dbParams::HumidityModeIn]    = db[HumidityModeIn];
  Params[dbParams::CO2ModeIn]         = db[CO2ModeIn];

  Params[dbParams::TemperatureModeOut] = db[TemperatureModeOut];
  Params[dbParams::HumidityModeOut]    = db[HumidityModeOut];
  Params[dbParams::CO2ModeOut]         = db[CO2ModeOut];
}

void loop() {
  WiFiConnector.tick();
  sett.tick();
  GreenHouse.Tick();
  HC.Tick();

  PIDHumidity.Tick(GreenHouse.Sensors.dht22in.Humidity.Value(), GreenHouse.Sensors.dht22in.IsValid());
  PIDTemperature.Tick(GreenHouse.Sensors.dht22in.Temperature.Value(), GreenHouse.Sensors.dht22in.IsValid());
  PIDCO2.Tick(CO2Diff(), GreenHouse.Sensors.mhz19in.IsValid());

  p.Tick();
}

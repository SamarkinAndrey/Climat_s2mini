#include "_alert.h"
#include "_common.h"
#include "_devices.h"
#include "_greenhouse.h"
#include "_params_state.h"
#include "_sensors.h"

_ParamState<bool> Params;

_GreenHouse GreenHouse;

struct _localParams {
  int  mhz19in_Range;
  int  mhz19out_Range;
  bool mhz19in_AutoCalibration;
  bool mhz19out_AutoCalibration;
};

_localParams localParams;

void WebUpdate() {
  //  debug.tprintln("WebUpdate()");

  sett.updater()
      .update(dbParams::TemperatureIn, GreenHouse.Sensors.dht22in.Temperature.Text)
      .update(dbParams::HumidityIn, GreenHouse.Sensors.dht22in.Humidity.Text)
      .update(dbParams::CO2In, GreenHouse.Sensors.mhz19in.CO2.Text)
#ifdef PLOT
      .updatePlot(H(PlotIn), VL{GreenHouse.Sensors.dht22in.Temperature.Value,
                                GreenHouse.Sensors.dht22in.Humidity.Value,
                                GreenHouse.Sensors.mhz19in.CO2.Value})
      .updatePlot(H(PlotOut), VL{GreenHouse.Sensors.dht22out.Temperature.Value,
                                 GreenHouse.Sensors.dht22out.Humidity.Value,
                                 GreenHouse.Sensors.mhz19out.CO2.Value})
#endif
      .update(H(log), logger);

  if (GreenHouse.Sensors.TemperatureSync.IsActive()) {
    char _buffer[50] = "⇄ ";
    strcat(_buffer, GreenHouse.Sensors.dht22out.Temperature.Text);

    sett.updater().update(dbParams::TemperatureOut, _buffer);
  } else {
    sett.updater().update(dbParams::TemperatureOut, GreenHouse.Sensors.dht22out.Temperature.Text);
  }

  if (GreenHouse.Sensors.HumiditySync.IsActive()) {
    char _buffer[50] = "⇄ ";
    strcat(_buffer, GreenHouse.Sensors.dht22out.Humidity.Text);

    sett.updater().update(dbParams::HumidityOut, _buffer);
  } else {
    sett.updater().update(dbParams::HumidityOut, GreenHouse.Sensors.dht22out.Humidity.Text);
  }

  if (GreenHouse.Sensors.CO2Sync.IsActive()) {
    char _buffer[50] = "⇄ ";
    strcat(_buffer, GreenHouse.Sensors.mhz19out.CO2.Text);

    sett.updater().update(dbParams::CO2Out, _buffer);
  } else {
    sett.updater().update(dbParams::CO2Out, GreenHouse.Sensors.mhz19out.CO2.Text);
  }

  if (GreenHouse.Sensors.TemperatureSync.IsActive())
    sett.updater().update(dbParams::TemperatureSyncPercent, GreenHouse.Sensors.TemperatureSync.getMillisLeftStr());

  if (GreenHouse.Sensors.HumiditySync.IsActive())
    sett.updater().update(dbParams::HumiditySyncPercent, GreenHouse.Sensors.HumiditySync.getMillisLeftStr());

  if (GreenHouse.Sensors.CO2Sync.IsActive())
    sett.updater().update(dbParams::CO2SyncPercent, GreenHouse.Sensors.CO2Sync.getMillisLeftStr());

  sett.updater().updateColor(dbParams::TemperatureIn, GreenHouse.Alerts.Temperature.getColor());
  sett.updater().updateColor(dbParams::HumidityIn, GreenHouse.Alerts.Humidity.getColor());
  sett.updater().updateColor(dbParams::CO2In, GreenHouse.Alerts.CO2.getColor());

  Params[dbParams::DHT22InExists]  = GreenHouse.Sensors.dht22in.IsActive();
  Params[dbParams::DHT22OutExists] = GreenHouse.Sensors.dht22out.IsActive();
  Params[dbParams::MHZ19InExists]  = GreenHouse.Sensors.mhz19in.IsActive();
  Params[dbParams::MHZ19OutExists] = GreenHouse.Sensors.mhz19out.IsActive();

  Params[dbParams::MHZ19InHeating]  = GreenHouse.Sensors.mhz19in.IsHeating();
  Params[dbParams::MHZ19OutHeating] = GreenHouse.Sensors.mhz19out.IsHeating();

  Params[dbParams::TemperatureIsSync] = GreenHouse.Sensors.TemperatureSync.IsActive();
  Params[dbParams::HumidityIsSync]    = GreenHouse.Sensors.HumiditySync.IsActive();
  Params[dbParams::CO2IsSync]         = GreenHouse.Sensors.CO2Sync.IsActive();

  if (Params.IsChanged()) {
    debug.tprintln("Params.IsChanged()");

    sett.reload();
  }
}

void AlertsUpdate() {
  //  debug.tprintln("AlertsUpdate()");

  _Alert &Temperature = GreenHouse.Alerts.Temperature;
  _Alert &Humidity    = GreenHouse.Alerts.Humidity;
  _Alert &CO2         = GreenHouse.Alerts.CO2;

  _Readings &TemperatureIn  = GreenHouse.Sensors.dht22in.Temperature;
  _Readings &HumidityIn     = GreenHouse.Sensors.dht22in.Humidity;
  _Readings &CO2In          = GreenHouse.Sensors.mhz19in.CO2;
  _Readings &TemperatureOut = GreenHouse.Sensors.dht22out.Temperature;
  _Readings &HumidityOut    = GreenHouse.Sensors.dht22out.Humidity;
  _Readings &CO2Out         = GreenHouse.Sensors.mhz19out.CO2;

  if (db[TemperatureControlEnabled].toBool() &&
      (TemperatureIn.Value > 0) &&
      ((TemperatureOut.Value < 1) ||
       (TemperatureIn.Value > TemperatureOut.Value)) &&
      (TemperatureIn.Value > db[TemperatureAlarmThreshold_HIGH].toInt())) {
    Temperature.setState(AS_HIGH, TemperatureIn.Value);
  } else if (db[TemperatureControlEnabled].toBool() &&
             (TemperatureIn.Value > 0) &&
             //  ((_s.dht22out.Temperature < 1) ||
             //   (TemperatureIn < _s.dht22out.Temperature)) &&
             (TemperatureIn.Value < db[TemperatureAlarmThreshold_LOW].toInt())) {
    Temperature.setState(AS_LOW, TemperatureIn.Value);
  } else {
    Temperature.setState(AS_NONE);
  }

  if (db[HumidityControlEnabled].toBool() &&
      (HumidityIn.Value > 0) &&
      ((HumidityOut.Value < 1) ||
       (HumidityIn.Value > HumidityOut.Value)) &&
      (HumidityIn.Value > db[HumidityAlarmThreshold_HIGH].toInt())) {
    Humidity.setState(AS_HIGH, HumidityIn.Value);
  } else if (db[HumidityControlEnabled].toBool() &&
             (HumidityIn.Value > 0) &&
             //  ((HumidityOut < 1) ||
             //   (HumidityIn < HumidityOut)) &&
             (HumidityIn.Value < db[HumidityAlarmThreshold_LOW].toInt())) {
    Humidity.setState(AS_LOW, HumidityIn.Value);
  } else {
    Humidity.setState(AS_NONE);
  }

  if (db[CO2ControlEnabled].toBool() &&
      (CO2In.Value > 0) &&
      ((CO2Out.Value < 1) ||
       (CO2In.Value > CO2Out.Value)) &&
      (CO2In.Value > db[CO2AlarmThreshold_HIGH].toInt())) {
    CO2.setState(AS_HIGH, CO2In.Value);
  } else {
    CO2.setState(AS_NONE);
  }

  GreenHouse.Sensors.setTemperatureHistoryEnabled(&Temperature, Temperature.getState() > 3);
  GreenHouse.Sensors.setHumidityHistoryEnabled(&Humidity, Humidity.getState() > 3);
  GreenHouse.Sensors.setCO2HistoryEnabled(&CO2, CO2.getState() > 3);
}

void DevicesUpdate() {
  //  debug.tprintln("DevicesUpdate()");

  _Device &FanMain    = GreenHouse.Devices.FanMain;
  _Device &FanInner   = GreenHouse.Devices.FanInner;
  _Device &Humidifier = GreenHouse.Devices.Humidifier;
  _Device &Heater     = GreenHouse.Devices.Heater;

  _Alert &Temperature = GreenHouse.Alerts.Temperature;
  _Alert &Humidity    = GreenHouse.Alerts.Humidity;
  _Alert &CO2         = GreenHouse.Alerts.CO2;

  switch (Temperature.getState()) {
    case AS_HIGH:
      FanMain.Start(&Temperature,
                    db[TemperatureFanDuration].toInt() * 1000UL,
                    db[TemperatureFanDelay].toInt() * 1000UL);
      break;

    case AS_LOW:
      Heater.Start(&Temperature,
                   db[TemperatureHeatingDuration].toInt() * 1000UL,
                   db[TemperatureHeatingDelay].toInt() * 1000UL);
      break;

    default:
      FanMain.Stop(&Temperature);
      Heater.Stop(&Temperature);
  }

  switch (Humidity.getState()) {
    case AS_HIGH:
      FanMain.Start(&Humidity,
                    db[HumidityFanDuration].toInt() * 1000UL,
                    db[HumidityFanDelay].toInt() * 1000UL);
      break;

    case AS_LOW:
      Humidifier.Start(&Humidity,
                       db[HumidityWettingDuration].toFloat() * 1000UL,
                       db[HumidityWettingDelay].toInt() * 1000UL);

      FanInner.Start(&Humidity);
      break;

    default:
      FanMain.Stop(&Humidity);
      Humidifier.Stop(&Humidity);
      FanInner.Stop(&Humidity);
  }

  switch (CO2.getState()) {
    case AS_HIGH:
      FanMain.Start(&CO2,
                    db[CO2FanDuration].toInt() * 1000UL,
                    db[CO2FanDelay].toInt() * 1000UL);
      break;

    default:
      FanMain.Stop(&CO2);
  }
}

void WebBuild(sets::Builder &b) {
  debug.tprintln("WebBuild()");

  if (b.build.isAction()) {
    if (b.build.id < (sizeof(dbParamsName) / sizeof(char *))) {
      debug.tprint(dbParamsName[b.build.id]);
    } else {
      debug.tprint(b.build.id);
    }

    debug.print(" = ");
    debug.println(b.build.value);

    if ((b.build.id == dbParams::TemperatureControlEnabled) ||
        (b.build.id == dbParams::HumidityControlEnabled) ||
        (b.build.id == dbParams::CO2ControlEnabled) ||
        (db[TemperatureControlEnabled].toBool() &&
         ((b.build.id == dbParams::TemperatureAlarmThreshold_LOW) ||
          (b.build.id == dbParams::TemperatureAlarmThreshold_HIGH) ||
          (b.build.id == dbParams::TemperatureFanDuration) ||
          (b.build.id == dbParams::TemperatureFanDelay) ||
          (b.build.id == dbParams::TemperatureHeatingDuration) ||
          (b.build.id == dbParams::TemperatureHeatingDelay))) ||
        (db[HumidityControlEnabled].toBool() &&
         ((b.build.id == dbParams::HumidityAlarmThreshold_LOW) ||
          (b.build.id == dbParams::HumidityAlarmThreshold_HIGH) ||
          (b.build.id == dbParams::HumidityFanDuration) ||
          (b.build.id == dbParams::HumidityFanDelay) ||
          (b.build.id == dbParams::HumidityWettingDuration) ||
          (b.build.id == dbParams::HumidityWettingDelay))) ||
        (db[CO2ControlEnabled].toBool() &&
         ((b.build.id == dbParams::CO2AlarmThreshold_HIGH) ||
          (b.build.id == dbParams::CO2FanDuration) ||
          (b.build.id == dbParams::CO2FanDelay)))) {
      GreenHouse.AlertsUpdate();
    }

    switch (b.build.id) {
      case dbParams::DebugMode:
        debug.setModeInt(b.build.value.toInt());
        break;

      case dbParams::SensorScanDelay:
        GreenHouse.setSensorUpdateInterval(b.build.value.toInt() * 1000UL);
        break;

      case dbParams::TemperatureInStackSize:
        GreenHouse.Sensors.dht22in.setTemperatureStackSize(b.build.value.toInt());
        break;

      case dbParams::HumidityInStackSize:
        GreenHouse.Sensors.dht22in.setHumidityStackSize(b.build.value.toInt());
        break;

      case dbParams::CO2InStackSize:
        GreenHouse.Sensors.mhz19in.setCO2StackSize(b.build.value.toInt());
        break;

      case dbParams::TemperatureOutStackSize:
        GreenHouse.Sensors.dht22out.setTemperatureStackSize(b.build.value.toInt());
        break;

      case dbParams::HumidityOutStackSize:
        GreenHouse.Sensors.dht22out.setHumidityStackSize(b.build.value.toInt());
        break;

      case dbParams::CO2OutStackSize:
        GreenHouse.Sensors.mhz19out.setCO2StackSize(b.build.value.toInt());
        break;

      case dbParams::TemperatureOutOffset:
        GreenHouse.Sensors.dht22out.Temperature.setOffset(b.build.value.toFloat());
        break;

      case dbParams::HumidityOutOffset:
        GreenHouse.Sensors.dht22out.Humidity.setOffset(b.build.value.toFloat());
        break;

      case dbParams::CO2OutOffset:
        GreenHouse.Sensors.mhz19out.CO2.setOffset(b.build.value.toFloat());
        break;

      case dbParams::CO2InRange:
        // GreenHouse.Sensors.mhz19in.setRange(b.build.value.toInt() == 0 ? 2000 : 5000);
        break;

      case dbParams::CO2InAutoCalibration:
        // GreenHouse.Sensors.mhz19in.setAutoCalibration(b.build.value.toBool());
        break;

      case dbParams::CO2OutRange:
        // GreenHouse.Sensors.mhz19out.setRange(b.build.value.toInt() == 0 ? 2000 : 5000);
        break;

      case dbParams::CO2OutAutoCalibration:
        // GreenHouse.Sensors.mhz19out.setAutoCalibration(b.build.value.toBool());
        break;
    }
  }

  // 🌡 💧⚙️💭💦♨♨️🌀✇☢✇🛠️🔗🌡🌡️💧🛠🌡

  if (b.beginGroup("Внутри")) {
#ifdef PLOT
    switch (db[DisplayModeTabIn].toInt()) {
      case 0:
#endif

        b.Label(dbParams::TemperatureIn, "🌡️ Температура", GreenHouse.Sensors.dht22in.Temperature.Text, GreenHouse.Alerts.Temperature.getColor());
        b.Label(dbParams::HumidityIn, "💧 Влажность", GreenHouse.Sensors.dht22in.Humidity.Text, GreenHouse.Alerts.Humidity.getColor());
        b.Label(dbParams::CO2In, "💭 CO2", GreenHouse.Sensors.mhz19in.CO2.Text, GreenHouse.Alerts.CO2.getColor());

#ifdef PLOT
        break;

      case 1:
        b.PlotRunning(H(PlotIn), "Температура;Влажность;CO2");
        break;
    }

    if (b.beginRow("")) {
      if (b.Tabs(dbParams::DisplayModeTabIn, "Показания; График"))
        b.reload();

      b.endRow();
    }
#endif

    b.endGroup();
  }

  if (b.beginGroup("Снаружи")) {
#ifdef PLOT
    switch (db[DisplayModeTabOut].toInt()) {
      case 0:
#endif

        if (GreenHouse.Sensors.TemperatureSync.IsActive()) {
          char _buffer[50] = "⇄ ";
          strcat(_buffer, GreenHouse.Sensors.dht22out.Temperature.Text);

          b.Label(dbParams::TemperatureOut, "🌡️ Температура", _buffer);
        } else {
          b.Label(dbParams::TemperatureOut, "🌡️ Температура", GreenHouse.Sensors.dht22out.Temperature.Text);
        }

        if (GreenHouse.Sensors.HumiditySync.IsActive()) {
          char _buffer[50] = "⇄ ";
          strcat(_buffer, GreenHouse.Sensors.dht22out.Humidity.Text);

          b.Label(dbParams::HumidityOut, "💧 Влажность", _buffer);
        } else {
          b.Label(dbParams::HumidityOut, "💧 Влажность", GreenHouse.Sensors.dht22out.Humidity.Text);
        }

        if (GreenHouse.Sensors.CO2Sync.IsActive()) {
          char _buffer[50] = "⇄ ";
          strcat(_buffer, GreenHouse.Sensors.mhz19out.CO2.Text);

          b.Label(dbParams::CO2Out, "💭 CO2", _buffer);
        } else {
          b.Label(dbParams::CO2Out, "💭 CO2", GreenHouse.Sensors.mhz19out.CO2.Text);
        }

#ifdef PLOT
        break;

      case 1:
        b.PlotRunning(H(PlotOut), "Температура;Влажность;CO2");
        break;
    }

    if (b.beginRow("")) {
      if (b.Tabs(dbParams::DisplayModeTabOut, "Показания; График"))
        b.reload();

      b.endRow();
    }
#endif

    b.endGroup();
  }

  if (b.beginGroup("🔗 Контроль")) {
    b.Switch(dbParams::TemperatureControlEnabled, "Температура");

    if (b.beginMenu("Настройки")) {
      b.Slider2(dbParams::TemperatureAlarmThreshold_LOW, dbParams::TemperatureAlarmThreshold_HIGH, "Поддерживать", 0, 50, 0.5, " °C");

      if (b.beginGroup("Вентиляция")) {
        b.Slider(dbParams::TemperatureFanDuration, "Продолжительность", 0, 60, 1, " сек.");
        b.Slider(dbParams::TemperatureFanDelay, "Пауза", 0, 60, 1, " сек.");
        b.endGroup();
      }
      if (b.beginGroup("Подогрев")) {
        b.Slider(dbParams::TemperatureHeatingDuration, "Продолжительность", 0, 60, 1, " мин.");
        b.Slider(dbParams::TemperatureHeatingDelay, "Пауза", 0, 60, 1, " мин.");
        b.endGroup();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup()) {
    b.Switch(dbParams::HumidityControlEnabled, "Влажность");

    if (b.beginMenu("Настройки")) {
      b.Slider2(dbParams::HumidityAlarmThreshold_LOW, dbParams::HumidityAlarmThreshold_HIGH, "Поддерживать", 0, 90, 0.5, " %");

      if (b.beginGroup("Вентиляция")) {
        b.Slider(dbParams::HumidityFanDuration, "Продолжительность", 0, 60, 1, " сек.");
        b.Slider(dbParams::HumidityFanDelay, "Пауза", 0, 60, 1, " сек.");
        b.endMenu();
      }
      if (b.beginGroup("Увлажнение")) {
        b.Slider(dbParams::HumidityWettingDuration, "Продолжительность", 0, 10, 0.1, " сек.");
        b.Slider(dbParams::HumidityWettingDelay, "Пауза", 0, 60, 1, " сек.");
        b.endMenu();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup()) {
    b.Switch(dbParams::CO2ControlEnabled, "CO2");

    if (b.beginMenu("Настройки")) {
      b.Slider(dbParams::CO2AlarmThreshold_HIGH, "Порог срабатывания", 400, CO2_MAX_RANGE, 50, " ppm");

      if (b.beginGroup("Вентиляция")) {
        b.Slider(dbParams::CO2FanDuration, "Продолжительность", 0, 60, 1, " сек.");
        b.Slider(dbParams::CO2FanDelay, "Пауза", 0, 60, 1, " сек.");
        b.endMenu();
      }
      b.endMenu();
    }
    b.endGroup();
  }

  if (b.beginGroup("🛠️ Настройки")) {
    if (b.beginMenu("Датчики")) {
      b.Slider(dbParams::SensorScanDelay, "Частота опроса", 2, 300, 1, " сек.");

      if (GreenHouse.Sensors.dht22in.IsActive()) {
        if (b.beginGroup(String(GreenHouse.Sensors.dht22in.getName()))) {
          b.Number(dbParams::TemperatureInStackSize, "Размер стека 🌡️", nullptr, 1, 100);
          b.Number(dbParams::HumidityInStackSize, "Размер стека 💧", nullptr, 1, 100);
          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.dht22out.IsActive()) {
        if (b.beginGroup(String(GreenHouse.Sensors.dht22out.getName()))) {
          b.Number(dbParams::TemperatureOutStackSize, "Размер стека 🌡️", nullptr, 1, 100);
          b.Number(dbParams::HumidityOutStackSize, "Размер стека 💧", nullptr, 1, 100);

          b.endGroup();
        }
      }

      if (GreenHouse.Sensors.mhz19in.IsActive()) {
        if (b.beginGroup(String(GreenHouse.Sensors.mhz19in.getName()))) {
          localParams.mhz19in_Range           = GreenHouse.Sensors.mhz19in.Range == 2000 ? 0 : 1;
          localParams.mhz19in_AutoCalibration = GreenHouse.Sensors.mhz19in.AutoCalibration;

          b.Number(dbParams::CO2InStackSize, "Размер стека 💭", nullptr, 1, 100);

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

      if (GreenHouse.Sensors.mhz19out.IsActive()) {
        if (b.beginGroup(String(GreenHouse.Sensors.mhz19out.getName()))) {
          localParams.mhz19out_Range           = GreenHouse.Sensors.mhz19out.Range == 2000 ? 0 : 1;
          localParams.mhz19out_AutoCalibration = GreenHouse.Sensors.mhz19out.AutoCalibration;

          b.Number(dbParams::CO2OutStackSize, "Размер стека 💭", nullptr, 1, 100);

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

      if (GreenHouse.Sensors.dht22out.IsActive() && GreenHouse.Sensors.dht22in.IsActive()) {
        if (b.beginGroup("Синхронизация внешних датчиков")) {
          _begin = true;

          if (GreenHouse.Sensors.TemperatureSync.IsActive()) {
            b.Label(dbParams::TemperatureSyncPercent, "Синхронизация 🌡️", GreenHouse.Sensors.TemperatureSync.getMillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::TemperatureSyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::TemperatureOutOffset, "Компенсация 🌡️");

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
          if (GreenHouse.Sensors.HumiditySync.IsActive()) {
            b.Label(dbParams::HumiditySyncPercent, "Синхронизация 💧", GreenHouse.Sensors.HumiditySync.getMillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::HumiditySyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::HumidityOutOffset, "Компенсация 💧");

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

      if (GreenHouse.Sensors.mhz19out.IsActive() && GreenHouse.Sensors.mhz19in.IsActive() &&
          !GreenHouse.Sensors.mhz19out.IsHeating() && !GreenHouse.Sensors.mhz19in.IsHeating()) {
        if (b.beginGroup((!_begin) ? "Синхронизация внешних датчиков" : "")) {
          if (GreenHouse.Sensors.CO2Sync.IsActive()) {
            b.Label(dbParams::CO2SyncPercent, "Синхронизация 💭", GreenHouse.Sensors.CO2Sync.getMillisLeftStr());

            if (b.Button("Отменить")) {
              sett.updater().update(dbParams::CO2SyncCancelConfirm, "Уверены?");
            }
          } else {
            b.Number(dbParams::CO2OutOffset, "Компенсация 💭");

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

    if (b.beginMenu("Система")) {
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
      if (b.Button("История, стек")) {
        logger.clear();

        logger.println("=== History Size ===");

        logger.printf("dht22in.Temperature.HistorySize = %d\n", GreenHouse.Sensors.dht22in.Temperature.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.dht22in.Temperature.History.MillisFromStr, GreenHouse.Sensors.dht22in.Temperature.History.MillisToStr);
        logger.printf("dht22in.Humidity.HistorySize = %d\n", GreenHouse.Sensors.dht22in.Humidity.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.dht22in.Humidity.History.MillisFromStr, GreenHouse.Sensors.dht22in.Humidity.History.MillisToStr);
        logger.printf("mhz19in.CO2.HistorySize = %d\n", GreenHouse.Sensors.mhz19in.CO2.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.mhz19in.CO2.History.MillisFromStr, GreenHouse.Sensors.mhz19in.CO2.History.MillisToStr);
        logger.printf("dht22out.Temperature.HistorySize = %d\n", GreenHouse.Sensors.dht22out.Temperature.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.dht22out.Temperature.History.MillisFromStr, GreenHouse.Sensors.dht22out.Temperature.History.MillisToStr);
        logger.printf("dht22out.Humidity.HistorySize = %d\n", GreenHouse.Sensors.dht22out.Humidity.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.dht22out.Humidity.History.MillisFromStr, GreenHouse.Sensors.dht22out.Humidity.History.MillisToStr);
        logger.printf("mhz19out.CO2.HistorySize = %d\n", GreenHouse.Sensors.mhz19out.CO2.History.getSize());
        logger.printf("millis: from %s to %s\n", GreenHouse.Sensors.mhz19out.CO2.History.MillisFromStr, GreenHouse.Sensors.mhz19out.CO2.History.MillisToStr);

        logger.println("~");
        logger.println("=== Stack Size ===");

        logger.printf("dht22in.Temperature.StackSize = %d\n", GreenHouse.Sensors.dht22in.Temperature.getStackSize());
        logger.printf("dht22in.Humidity.StackSize = %d\n", GreenHouse.Sensors.dht22in.Humidity.getStackSize());
        logger.printf("mhz19in.CO2.StackSize = %d\n", GreenHouse.Sensors.mhz19in.CO2.getStackSize());
        logger.printf("dht22out.Temperature.StackSize = %d\n", GreenHouse.Sensors.dht22out.Temperature.getStackSize());
        logger.printf("dht22out.Humidity.StackSize = %d\n", GreenHouse.Sensors.dht22out.Humidity.getStackSize());
        logger.printf("mhz19out.CO2.StackSize = %d\n", GreenHouse.Sensors.mhz19out.CO2.getStackSize());

        logger.println("~");
        logger.println("=== Readings Offset ===");

        logger.printf("dht22in.Temperature.Offset = %.4f\n", GreenHouse.Sensors.dht22in.getTemperatureOffset());
        logger.printf("dht22in.Humidity.Offset = %.4f\n", GreenHouse.Sensors.dht22in.getHumidityOffset());
        logger.printf("mhz19in.CO2.Offset = %.4f\n", GreenHouse.Sensors.mhz19in.getCO2Offset());
        logger.printf("dht22out.Temperature.Offset = %.4f\n", GreenHouse.Sensors.dht22out.getTemperatureOffset());
        logger.printf("dht22out.Humidity.Offset = %.4f\n", GreenHouse.Sensors.dht22out.getHumidityOffset());
        logger.printf("mhz19out.CO2.Offset = %.4f\n", GreenHouse.Sensors.mhz19out.getCO2Offset());

        sett.updater().update(H(log), logger);
      }

      if (b.Button("Статистика за 30мин.")) {
        _IntervalData Tin = GreenHouse.Sensors.dht22in.Temperature.History.getIntervalStat(1800);
        _IntervalData Hin = GreenHouse.Sensors.dht22in.Humidity.History.getIntervalStat(1800);
        _IntervalData Cin = GreenHouse.Sensors.mhz19in.CO2.History.getIntervalStat(1800);

        _IntervalData Tout = GreenHouse.Sensors.dht22out.Temperature.History.getIntervalStat(1800);
        _IntervalData Hout = GreenHouse.Sensors.dht22out.Humidity.History.getIntervalStat(1800);
        _IntervalData Cout = GreenHouse.Sensors.mhz19out.CO2.History.getIntervalStat(1800);

        logger.clear();

        logger.println("=== Temperature Inner ===");

        logger.printf("TimeFrom: %s\n", Tin.MillisFromStr);
        logger.printf("TimeTo: %s\n", Tin.MillisToStr);
        logger.printf("ValueFrom :%.1f\n", Tin.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Tin.ValueTo);
        logger.printf("ValueMin: %.1f\n", Tin.ValueMin);
        logger.printf("ValueMax: %.1f\n", Tin.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Tin.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Tin.ValueUp);
        logger.printf("ValueDown: %.1f\n", Tin.ValueDown);
        logger.printf("Vibration: %.1f\n", Tin.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Tin.VibrationPercent);

        logger.println("~");
        logger.println("=== Humidity Inner ===");

        logger.printf("TimeFrom: %s\n", Hin.MillisFromStr);
        logger.printf("TimeTo: %s\n", Hin.MillisToStr);
        logger.printf("ValueFrom: %.1f\n", Hin.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Hin.ValueTo);
        logger.printf("ValueMin: %.1f\n", Hin.ValueMin);
        logger.printf("ValueMax: %.1f\n", Hin.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Hin.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Hin.ValueUp);
        logger.printf("ValueDown: %.1f\n", Hin.ValueDown);
        logger.printf("Vibration: %.1f\n", Hin.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Hin.VibrationPercent);

        logger.println("~");
        logger.println("=== CO2 Inner ===");

        logger.printf("TimeFrom: %s\n", Cin.MillisFromStr);
        logger.printf("TimeTo: %s\n", Cin.MillisToStr);
        logger.printf("ValueFrom: %.1f\n", Cin.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Cin.ValueTo);
        logger.printf("ValueMin: %.1f\n", Cin.ValueMin);
        logger.printf("ValueMax: %.1f\n", Cin.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Cin.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Cin.ValueUp);
        logger.printf("ValueDown: %.1f\n", Cin.ValueDown);
        logger.printf("Vibration: %.1f\n", Cin.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Cin.VibrationPercent);

        logger.println("~");
        logger.println("=== Temperature Outer ===");

        logger.printf("TimeFrom: %s\n", Tout.MillisFromStr);
        logger.printf("TimeTo: %s\n", Tout.MillisToStr);
        logger.printf("ValueFrom :%.1f\n", Tout.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Tout.ValueTo);
        logger.printf("ValueMin: %.1f\n", Tout.ValueMin);
        logger.printf("ValueMax: %.1f\n", Tout.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Tout.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Tout.ValueUp);
        logger.printf("ValueDown: %.1f\n", Tout.ValueDown);
        logger.printf("Vibration: %.1f\n", Tout.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Tout.VibrationPercent);

        logger.println("~");
        logger.println("=== Humidity Outer ===");

        logger.printf("TimeFrom: %s\n", Hout.MillisFromStr);
        logger.printf("TimeTo: %s\n", Hout.MillisToStr);
        logger.printf("ValueFrom: %.1f\n", Hout.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Hout.ValueTo);
        logger.printf("ValueMin: %.1f\n", Hout.ValueMin);
        logger.printf("ValueMax: %.1f\n", Hout.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Hout.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Hout.ValueUp);
        logger.printf("ValueDown: %.1f\n", Hout.ValueDown);
        logger.printf("Vibration: %.1f\n", Hout.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Hout.VibrationPercent);

        logger.println("~");
        logger.println("=== CO2 Outer ===");

        logger.printf("TimeFrom: %s\n", Cout.MillisFromStr);
        logger.printf("TimeTo: %s\n", Cout.MillisToStr);
        logger.printf("ValueFrom: %.1f\n", Cout.ValueFrom);
        logger.printf("ValueTo: %.1f\n", Cout.ValueTo);
        logger.printf("ValueMin: %.1f\n", Cout.ValueMin);
        logger.printf("ValueMax: %.1f\n", Cout.ValueMax);
        logger.printf("ValueAvg: %.1f\n", Cout.ValueAvg);
        logger.printf("ValueUp: %.1f\n", Cout.ValueUp);
        logger.printf("ValueDown: %.1f\n", Cout.ValueDown);
        logger.printf("Vibration: %.1f\n", Cout.Vibration);
        logger.printf("Vibration(%%): %.1f\n", Cout.VibrationPercent);

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
      if (GreenHouse.Sensors.mhz19in.IsActive()) {
        GreenHouse.Sensors.mhz19in.Calibrate400();
      }
    }
  }

  if (b.Confirm(dbParams::CO2OutCalibrationConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      if (GreenHouse.Sensors.mhz19out.IsActive()) {
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
      GreenHouse.Sensors.TemperatureSync.Start();
    }
  }

  if (b.Confirm(dbParams::HumiditySyncConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.HumiditySync.Start();
    }
  }

  if (b.Confirm(dbParams::CO2SyncConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.CO2Sync.Start();
    }
  }

  if (b.Confirm(dbParams::TemperatureSyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.TemperatureSync.Cancel();
    }
  }

  if (b.Confirm(dbParams::HumiditySyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.HumiditySync.Cancel();
    }
  }

  if (b.Confirm(dbParams::CO2SyncCancelConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.CO2Sync.Cancel();
    }
  }

  if (b.Confirm(dbParams::TemperatureClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.dht22out.setTemperatureOffset(0);
      db[TemperatureOutOffset] = 0;
    }
  }

  if (b.Confirm(dbParams::HumidityClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.dht22out.setHumidityOffset(0);
      db[HumidityOutOffset] = 0;
    }
  }

  if (b.Confirm(dbParams::CO2ClearOffsetConfirm, "Уверены?")) {
    if (b.build.value.toBool()) {
      GreenHouse.Sensors.mhz19out.setCO2Offset(0);
      db[CO2OutOffset] = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  setStampZone(3);

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

  LittleFS.begin(true);

  db.begin();

  db.init(dbParams::SensorScanDelay, 2);

  db.init(dbParams::TemperatureControlEnabled, (bool)0);
  db.init(dbParams::TemperatureAlarmThreshold_LOW, (float)20);
  db.init(dbParams::TemperatureAlarmThreshold_HIGH, (float)30);
  db.init(dbParams::TemperatureFanDuration, (int)5);
  db.init(dbParams::TemperatureFanDelay, (int)15);
  db.init(dbParams::TemperatureHeatingDuration, (int)5);
  db.init(dbParams::TemperatureHeatingDelay, (int)15);

  db.init(dbParams::HumidityControlEnabled, (bool)0);
  db.init(dbParams::HumidityAlarmThreshold_LOW, (float)60);
  db.init(dbParams::HumidityAlarmThreshold_HIGH, (float)90);
  db.init(dbParams::HumidityFanDuration, (int)5);
  db.init(dbParams::HumidityFanDelay, (int)15);
  db.init(dbParams::HumidityWettingDuration, (float)5);
  db.init(dbParams::HumidityWettingDelay, (int)15);

  db.init(dbParams::CO2ControlEnabled, (bool)0);
  db.init(dbParams::CO2AlarmThreshold_HIGH, (float)1500);
  db.init(dbParams::CO2FanDuration, (int)5);
  db.init(dbParams::CO2FanDelay, (int)15);

  db.init(dbParams::TemperatureInStackSize, (size_t)1);
  db.init(dbParams::HumidityInStackSize, (size_t)1);
  db.init(dbParams::CO2InStackSize, (size_t)1);

  db.init(dbParams::TemperatureOutStackSize, (size_t)1);
  db.init(dbParams::HumidityOutStackSize, (size_t)1);
  db.init(dbParams::CO2OutStackSize, (size_t)1);

  db.init(dbParams::TemperatureOutOffset, (float)0);
  db.init(dbParams::HumidityOutOffset, (float)0);
  db.init(dbParams::CO2OutOffset, (float)0);

  db.init(dbParams::DebugMode, (uint8_t)0);

#ifdef PLOT
  db.init(dbParams::DisplayModeTabIn, 0);
  db.init(dbParams::DisplayModeTabOut, 0);
#endif

  debug.setModeInt(db[dbParams::DebugMode].toInt());

  GreenHouse.setSensorUpdateInterval(db[SensorScanDelay].toInt() * 1000UL);

  GreenHouse.Sensors.dht22in.setTemperatureStackSize(db[TemperatureInStackSize].toInt());
  GreenHouse.Sensors.dht22in.setHumidityStackSize(db[HumidityInStackSize].toInt());
  GreenHouse.Sensors.mhz19in.setCO2StackSize(db[CO2InStackSize].toInt());

  GreenHouse.Sensors.dht22out.setTemperatureStackSize(db[TemperatureOutStackSize].toInt());
  GreenHouse.Sensors.dht22out.setHumidityStackSize(db[HumidityOutStackSize].toInt());
  GreenHouse.Sensors.mhz19out.setCO2StackSize(db[CO2OutStackSize].toInt());

  GreenHouse.Sensors.dht22out.setTemperatureOffset(db[TemperatureOutOffset].toFloat());
  GreenHouse.Sensors.dht22out.setHumidityOffset(db[HumidityOutOffset].toFloat());
  GreenHouse.Sensors.mhz19out.setCO2Offset(db[CO2OutOffset].toFloat());

  GreenHouse.Sensors.OnTemperatureBeforeSync([]() { db[TemperatureOutOffset] = 0; });
  GreenHouse.Sensors.OnHumidityBeforeSync([]() { db[HumidityOutOffset] = 0; });
  GreenHouse.Sensors.OnCO2BeforeSync([]() { db[CO2OutOffset] = 0; });

  GreenHouse.Sensors.OnTemperatureAfterSync([]() { db[TemperatureOutOffset] = GreenHouse.Sensors.dht22out.getTemperatureOffset(); });
  GreenHouse.Sensors.OnHumidityAfterSync([]() { db[HumidityOutOffset] = GreenHouse.Sensors.dht22out.getHumidityOffset(); });
  GreenHouse.Sensors.OnCO2AfterSync([]() { db[CO2OutOffset] = GreenHouse.Sensors.mhz19out.getCO2Offset(); });

  GreenHouse.OnAlertsUpdate(AlertsUpdate);
  GreenHouse.OnDevicesUpdate(DevicesUpdate);
  GreenHouse.OnWebUpdate(WebUpdate);

  Params[dbParams::DHT22InExists]  = false;
  Params[dbParams::DHT22OutExists] = false;
  Params[dbParams::MHZ19InExists]  = false;
  Params[dbParams::MHZ19OutExists] = false;

  Params[dbParams::MHZ19InHeating]  = false;
  Params[dbParams::MHZ19OutHeating] = false;

  Params[dbParams::TemperatureIsSync] = false;
  Params[dbParams::HumidityIsSync]    = false;
  Params[dbParams::CO2IsSync]         = false;
}

void loop() {
  WiFiConnector.tick();
  sett.tick();
  GreenHouse.Tick();
}

#pragma once

#include <DHT.h>
#include <MHZ19.h>
#include <Table.h>

#include "_classtype.h"
#include "_common.h"

#define AM2320_TEMPERATURE_ACCURACY 0.5f
#define AM2320_HUMIDITY_ACCURACY    3.0f
#define MHZ19_CO2_ACCURACY          50.0f

namespace cmp {

  inline float CalculateThreshold(float Accuracy1, float Accuracy2) {
    Accuracy1 = fabsf(Accuracy1);
    Accuracy2 = fabsf(Accuracy2);

    return sqrtf(Accuracy1 * Accuracy1 + Accuracy2 * Accuracy2) + std::numeric_limits<float>::epsilon();
  }

  inline bool IsEqual(float Value1, float Accuracy1,
                      float Value2, float Accuracy2) {
    if (!std::isfinite(Value1) || !std::isfinite(Value2))
      return false;

    return fabsf(Value1 - Value2) <= CalculateThreshold(Accuracy1, Accuracy2);
  }

  inline bool IsGreater(float Value1, float Accuracy1,
                        float Value2, float Accuracy2) {
    if (!std::isfinite(Value1) || !std::isfinite(Value2))
      return false;

    return (Value1 - Value2) > CalculateThreshold(Accuracy1, Accuracy2);
  }

  inline bool IsSmaller(float Value1, float Accuracy1,
                        float Value2, float Accuracy2) {
    if (!std::isfinite(Value1) || !std::isfinite(Value2))
      return false;

    return (Value2 - Value1) > CalculateThreshold(Accuracy1, Accuracy2);
  }

  inline int Compare(float Value1, float Accuracy1,
                     float Value2, float Accuracy2) {
    if (!std::isfinite(Value1) || !std::isfinite(Value2))
      return 0;

    float _diff      = Value1 - Value2;
    float _threshold = CalculateThreshold(Accuracy1, Accuracy2);

    if (_diff > _threshold)
      return 1;
    if (_diff < -_threshold)
      return -1;
    return 0;
  }

  inline bool IsGreaterOrEqual(float Value1, float Accuracy1, float Value2, float Accuracy2) {
    return !IsSmaller(Value1, Accuracy1, Value2, Accuracy2);
  }

  inline bool IsSmallerOrEqual(float Value1, float Accuracy1, float Value2, float Accuracy2) {
    return !IsGreater(Value1, Accuracy1, Value2, Accuracy2);
  }

} // namespace cmp

class _Readings;

class _ReadingsStack : public _ClassType, public _ClassOwner<_Readings> {
private:
  float *_stack    = nullptr;
  size_t _size_max = 0;
  size_t _size     = 0;
  size_t _start    = 0;
  float  _sum      = 0;

  float _Value = 0;

  void _setSize(size_t size_max = 0) {
    if (IsEnabled() ? (size_max == _size_max) : (size_max < 1))
      return;

    if (size_max < 1) {
      _Destroy();

      clear();

      _size_max = 0;

      return;
    }

    float *new_stack = new (std::nothrow) float[size_max];

    if (!new_stack)
      return;

    size_t new_size = min(_size, size_max);
    float  new_sum  = 0;

    for (size_t i = 0; i < new_size; i++) {
      new_stack[i] = _stack[(_start + i) % _size_max];
      new_sum += new_stack[i];
    }

    _Destroy();

    _stack    = new_stack;
    _size_max = size_max;
    _start    = 0;
    _size     = new_size;
    _sum      = new_sum;

    _Value = (_size > 0) ? _sum / static_cast<float>(_size) : 0;
  }

  void _Destroy() {
    if (_stack)
      delete[] _stack;

    _stack = nullptr;
  }

public:
  _ReadingsStack(_Readings &Readings, size_t size = 0) : _ClassOwner<_Readings>(Readings) {
    _setSize(size);

    setMainType(_MainType::Stack);
  }

  ~_ReadingsStack() {
    _Destroy();
  }

  void clear() {
    _start = 0;
    _size  = 0;
    _sum   = 0;

    _Value = 0;
  }

  bool empty() {
    return (_size < 1);
  }

  size_t size() {
    return _size;
  }

  float putValue(float value) {
    if (!IsEnabled() || !std::isfinite(value))
      return IsEnabled() ? _Value : value;

    if (_size < _size_max) {
      _stack[(_start + _size) % _size_max] = value;
      _sum += value;
      _size++;
    } else {
      _sum -= _stack[_start];
      _sum += value;
      _stack[_start] = value;

      _start = (_start + 1) % _size_max;
    }

    _Value = (_size > 0) ? _sum / static_cast<float>(_size) : 0;

    return _Value;
  }

  float Value() {
    return _Value;
  }

  void setSize(size_t size = 0) {
    _setSize(size);
  }

  size_t Size() {
    return _size_max;
  }

  bool IsEnabled() {
    return (_stack);
  }
};

class _ReadingsStat : public _ClassType, public _ClassOwner<_Readings> {
public:
  using _OnEnabledChange   = std::function<void(_ReadingsStat &Stat, bool IsEnabled)>;
  using _OnDirectionChange = std::function<void(_ReadingsStat &Stat, float Old, float New, bool Increase)>;
  using _OnValueChange     = std::function<void(_ReadingsStat &Stat, float Old, float New, bool Increase, bool DirectionChanged)>;
  using _OnValueAdd        = std::function<void(_ReadingsStat &Stat, float Value, ulong LastChangeMillisElapsed)>;

private:
  struct _ChangeEvent {
    float Old     = 0;
    float New     = 0;
    float Diff    = 0;
    ulong Millis  = 0;
    bool  IsValid = false;

    void clear() {
      Old     = 0;
      New     = 0;
      Diff    = 0;
      Millis  = 0;
      IsValid = false;
    }
  };

  struct _ChangeEvents {
    _ChangeEvent Change   = {};
    _ChangeEvent Revert   = {};
    _ChangeEvent Increase = {};
    _ChangeEvent Decrease = {};

    void clear() {
      Change.clear();
      Revert.clear();
      Increase.clear();
      Decrease.clear();
    }
  };

  float _Hysteresis = 0;

  bool _IsEnabled = false;
  // bool _IsWaiting = false;

  _OnEnabledChange   _on_enabled_change   = nullptr;
  _OnDirectionChange _on_direction_change = nullptr;
  _OnValueChange     _on_value_change     = nullptr;
  _OnValueAdd        _on_value_add        = nullptr;

  // void _setWaiting(bool Value) {
  //   if (_IsWaiting == Value)
  //     return;

  //   _IsWaiting = Value;

  //   debug.tprintf("%s.IsWaiting = %s\n", Name(), _IsWaiting ? "true" : "false");

  //   if (!_IsEnabled && !_IsWaiting && _on_enabled_change)
  //     _on_enabled_change(*this, false);
  // }

public:
  _ChangeEvents Events;

  ulong MillisFrom = 0;
  ulong MillisTo   = 0;

  ulong Duration = 0;

  float ValueFrom = 0;
  float ValueTo   = 0;

  float Min = 0;
  float Max = 0;

  ulong MinMillis = 0;
  ulong MaxMillis = 0;

  float Diff           = 0;
  float DiffHysteresis = 0;

  float TotalDiff        = 0;
  float TotalDiffPercent = 0;

  float Sum = 0;
  float Avg = 0;

  float Incrase = 0;
  float Decrase = 0;

  uint64_t Count = 0;

  bool IsValid = false;

  _ReadingsStat(_Readings &Readings) : _ClassOwner<_Readings>(Readings) {
    setMainType(_MainType::Stat);
  }

  void clear() {
    MillisFrom = 0;
    MillisTo   = 0;

    Duration = 0;

    ValueFrom = 0;
    ValueTo   = 0;

    Min = 0;
    Max = 0;

    MinMillis = 0;
    MaxMillis = 0;

    Diff           = 0;
    DiffHysteresis = 0;

    TotalDiff        = 0;
    TotalDiffPercent = 0;

    Sum = 0;
    Avg = 0;

    Incrase = 0;
    Decrase = 0;

    Count = 0;

    IsValid = false;

    Events.clear();
  }

  void Reload() {
    clear();
  }

  void putValue(float Value) {
    if (!_IsEnabled || !std::isfinite(Value))
      return;

    // if (!_IsEnabled && !_IsWaiting)
    //   return;

    // if (!std::isfinite(Value)) {
    //   if (_IsWaiting)
    //     _setWaiting(false);

    //   return;
    // }

    if (((Value > 0) && (Sum > std::numeric_limits<float>::max() - Value)) ||
        ((Value < 0) && (Sum < std::numeric_limits<float>::lowest() - Value))) {
      clear();
    }

    ulong _millis = millis();

    Count++;

    if (!IsValid) {
      MillisFrom = _millis;
      ValueFrom  = Value;
      Min        = Value;
      MinMillis  = _millis;
      Max        = Value;
      MaxMillis  = _millis;
    }

    MillisTo = _millis;
    ValueTo  = Value;

    Duration = MillisTo - MillisFrom;

    if (Value < Min) {
      Min       = Value;
      MinMillis = _millis;
    }

    if (Value > Max) {
      Max       = Value;
      MaxMillis = _millis;
    }

    Diff           = ValueTo - ValueFrom;
    DiffHysteresis = (fabsf(Diff) > _Hysteresis) ? Diff : 0;

    TotalDiff        = Max - Min;
    TotalDiffPercent = (Max != 0) ? TotalDiff * (100.0 / Max) : 0;

    Sum += Value;
    Avg = (Count > 0) ? Sum / static_cast<float>(Count) : 0;

    if (!IsValid) {
      Events.Change.New = Value;
    } else {
      float _diff     = Value - Events.Change.New;
      float _diff_abs = fabsf(_diff);

      if (_diff_abs > _Hysteresis) {
        bool IsRevert = false;

        if (Events.Change.IsValid && (Events.Change.Diff * _diff) < 0) {
          IsRevert = true;

          Events.Revert.Old     = Events.Change.New;
          Events.Revert.New     = Value;
          Events.Revert.Diff    = _diff;
          Events.Revert.Millis  = _millis;
          Events.Revert.IsValid = true;

          if (_on_direction_change)
            _on_direction_change(*this, Events.Change.New, Value, Value > Events.Change.New);
        }

        if (Value > Events.Change.New) {
          Events.Increase.Old     = Events.Change.New;
          Events.Increase.New     = Value;
          Events.Increase.Diff    = _diff;
          Events.Increase.Millis  = _millis;
          Events.Increase.IsValid = true;

          Incrase += _diff;
        }

        if (Value < Events.Change.New) {
          Events.Decrease.Old     = Events.Change.New;
          Events.Decrease.New     = Value;
          Events.Decrease.Diff    = _diff;
          Events.Decrease.Millis  = _millis;
          Events.Decrease.IsValid = true;

          Decrase += _diff;
        }

        Events.Change.Old     = Events.Change.New;
        Events.Change.New     = Value;
        Events.Change.Diff    = _diff;
        Events.Change.Millis  = _millis;
        Events.Change.IsValid = true;

        if (_on_value_change)
          _on_value_change(*this, Events.Change.Old, Events.Change.New, Events.Change.New > Events.Change.Old, IsRevert);
      }
    }

    IsValid = true;

    if (_on_value_add)
      _on_value_add(*this, Value, LastChangeMillisElapsed());

    // if (_IsWaiting && (Events.Change.IsRevert || (LastChangeMillisElapsed() > 5000)))
    //   _setWaiting(false);
  }

  bool IsUp(ulong Period = 0) {
    return Events.Change.IsValid && (Events.Change.Diff > 0) && ((Period > 0) ? (millis() - Events.Change.Millis) <= Period : true);
  }

  bool IsDown(ulong Period = 0) {
    return Events.Change.IsValid && (Events.Change.Diff < 0) && ((Period > 0) ? (millis() - Events.Change.Millis) <= Period : true);
  }

  float IsUpBy() {
    return (IsValid && Diff > 0) ? Diff : 0;
  }

  float IsDownBy() {
    return (IsValid && Diff < 0) ? fabsf(Diff) : 0;
  }

  float AbsoluteUp() {
    return (IsValid) ? Incrase : 0;
  }

  float AbsoluteDown() {
    return (IsValid) ? fabsf(Decrase) : 0;
  }

  bool IsChanged(ulong Period = 0) {
    return Events.Change.IsValid && ((Period > 0) ? (millis() - Events.Change.Millis) <= Period : false);
  }

  ulong LastChangeMillisElapsed() {
    return Events.Change.IsValid ? millis() - Events.Change.Millis : 0;
  }

  void setHysteresis(float Value) {
    _Hysteresis = Value;
  }

  float Hysteresis() {
    return _Hysteresis;
  }

  void setEnabled(bool Value) {
    if (_IsEnabled == Value)
      return;

    if (Value)
      clear();

    _IsEnabled = Value;

    // if (_IsEnabled && _on_enabled_change)
    //   _on_enabled_change(*this, true);

    debug.tprintf("%s.IsEnabled = %s\n", Name(), _IsEnabled ? "true" : "false");

    // _setWaiting(!Value);
  }

  bool IsEnabled() {
    // return _IsEnabled || _IsWaiting;
    return _IsEnabled;
  }

  // void OnEnabledChange(_OnEnabledChange cb) {
  //   if (cb)
  //     _on_enabled_change = cb;
  // }

  void OnDirectionChange(_OnDirectionChange cb) {
    if (cb)
      _on_direction_change = cb;
  }

  void OnValueChange(_OnValueChange cb) {
    if (cb)
      _on_value_change = cb;
  }

  void OnValueAdd(_OnValueAdd cb) {
    if (cb)
      _on_value_add = cb;
  }
};

class _ReadingsHistory : public _ClassType, public _ClassOwner<_Readings> {
private:
  Table _All;
  Table _New;

  size_t _StoreMinutes = 0;

public:
  _ReadingsHistory(_Readings &Readings, size_t StoreMinutes = 0) : _ClassOwner<_Readings>(Readings) {
    setStoreMinutes(StoreMinutes);

    _All.init(2, cell_t::Uint64, cell_t::Float);
    _New.init(2, cell_t::Uint64, cell_t::Float);

    setMainType(_MainType::History);
  }

  void putValue(float Value) {
    if (_StoreMinutes < 1 || !std::isfinite(Value) || !sett.rtc.synced())
      return;

    uint64_t _unixMs = sett.rtc.getUnixMs();

    _New.append(_unixMs, Value);

    _All.append(_unixMs, Value);
    while ((_unixMs - _All[0][0].toInt64()) > (static_cast<uint64_t>(_StoreMinutes) * 60000ull))
      _All.remove(0);
  }

  void clear() {
    _All.removeAll();
    _New.removeAll();
  }

  bool Enabled() {
    return _StoreMinutes > 0;
  }

  void setStoreMinutes(size_t Value) {
    _StoreMinutes = Value;

    if (_StoreMinutes < 1)
      clear();
  }

  size_t StoreMinutes() {
    return _StoreMinutes;
  }

  Table &All() {
    return _All;
  }

  const Table &All() const {
    return _All;
  }

  Table &New() {
    return _New;
  }

  const Table &New() const {
    return _New;
  }
};

class _SensorCustom;

class _Readings : public _ClassType, public _ClassOwner<_SensorCustom>, public _UsingIniciator {
public:
  using _OnGetAccuracy = std::function<float(float Accuracy)>;
  using _OnGetText     = std::function<void(char *Text)>;
  using _OnGetPrefix   = std::function<void(char *Prefix)>;
  using _OnGetPostfix  = std::function<void(char *Postfix)>;

private:
  float _Offset      = 0;
  float _Accuracy    = 0;
  float _Value       = 0;
  char  _Text[50]    = "";
  char  _Prefix[10]  = "";
  char  _Postfix[10] = "";

  _OnGetAccuracy _on_get_accuracy = nullptr;
  _OnGetText     _on_get_text     = nullptr;
  _OnGetPrefix   _on_get_prefix   = nullptr;
  _OnGetPostfix  _on_get_postfix  = nullptr;

  void _getText() {
    if (_on_get_text)
      _on_get_text(_Text);
  }

  void _getPrefix() {
    if (_on_get_prefix) {
      char _buf[10] = "";

      _on_get_prefix(_buf);

      if (strlen(_buf) > 0)
        snprintf(_Prefix, sizeof(_Prefix), "%s ", _buf);
      else
        _Prefix[0] = '\0';
    }
  }

  void _getPostfix() {
    if (_on_get_postfix) {
      char _buf[10] = "";

      _on_get_postfix(_buf);

      if (strlen(_buf) > 0)
        snprintf(_Postfix, sizeof(_Postfix), " %s", _buf);
      else
        _Postfix[0] = '\0';
    }
  }

public:
  _ReadingsHistory History;
  _ReadingsStack   Stack;
  _ReadingsStat    Stat;

  _Readings(_SensorCustom &Sensor, size_t HistorySize = 0, size_t StackSize = 0)
      : _ClassOwner<_SensorCustom>(Sensor),
        History(*this, HistorySize),
        Stack(*this, StackSize),
        Stat(*this) {
    setMainType(_MainType::Readings);
  }

  void Clear() {
    _Value      = 0;
    _Text[0]    = '\0';
    _Prefix[0]  = '\0';
    _Postfix[0] = '\0';

    History.clear();
    Stack.clear();
    Stat.clear();
  }

  void putValue(float Value) {
    _Value = Stack.putValue(Value + _Offset);

    History.putValue(_Value);
    Stat.putValue(_Value);
  }

  float Value() {
    return _Value;
  }

  void setPrefix(const char *Prefix) {
    char _buf[10] = "";

    strncpy(_buf, Prefix, sizeof(_buf) - 1);

    if (strlen(_buf) > 0) {
      snprintf(_Prefix, sizeof(_Prefix), "%s ", _buf);
    } else
      _Prefix[0] = '\0';
  }

  char *Prefix() {
    _getPrefix();

    return _Prefix;
  }

  void setPostfix(const char *Postfix) {
    char _buf[10] = "";

    strncpy(_buf, Postfix, sizeof(_buf) - 1);

    if (strlen(_buf) > 0) {
      snprintf(_Postfix, sizeof(_Postfix), " %s", _buf);
    } else
      _Postfix[0] = '\0';
  }

  char *Postfix() {
    _getPostfix();

    return _Postfix;
  }

  void setText(const char *Text) {
    strcpy(_Text, Text);
  }

  char *Text() {
    _getText();

    return _Text;
  }

  void setOffset(float Value = 0) {
    _Offset = Value;
  }

  float Offset() {
    return _Offset;
  }

  void setAccuracy(float Value = 0) {
    _Accuracy = Value;
  }

  float Accuracy() {
    if (_on_get_accuracy)
      return _on_get_accuracy(_Accuracy);

    return _Accuracy;
  }

  void OnGetAccuracy(_OnGetAccuracy cb) {
    if (cb)
      _on_get_accuracy = cb;
  }

  void OnGetText(_OnGetText cb) {
    if (cb)
      _on_get_text = cb;
  }

  void OnGetPrefix(_OnGetPrefix cb) {
    if (cb)
      _on_get_prefix = cb;
  }

  void OnGetPostfix(_OnGetPostfix cb) {
    if (cb)
      _on_get_postfix = cb;
  }

  int Compare(float value = 0, float accuracy = 0) {
    return cmp::Compare(Value(), Accuracy(), value, accuracy);
  }

  bool EqualTo(float value = 0, float accuracy = 0) {
    return cmp::IsEqual(Value(), Accuracy(), value, accuracy);
  }

  bool GreaterThen(float value = 0, float accuracy = 0) {
    return cmp::IsGreater(Value(), Accuracy(), value, accuracy);
  }

  bool GreaterOrEqualTo(float value = 0, float accuracy = 0) {
    return cmp::IsGreaterOrEqual(Value(), Accuracy(), value, accuracy);
  }

  bool SmallerThen(float value = 0, float accuracy = 0) {
    return cmp::IsSmaller(Value(), Accuracy(), value, accuracy);
  }

  bool SmallerOrEqualTo(float value = 0, float accuracy = 0) {
    return cmp::IsSmallerOrEqual(Value(), Accuracy(), value, accuracy);
  }

  int Compare(_Readings &Readings) {
    return Compare(Readings.Value(), Readings.Accuracy());
  }

  bool EqualTo(_Readings &Readings) {
    return EqualTo(Readings.Value(), Readings.Accuracy());
  }

  bool GreaterThen(_Readings &Readings) {
    return GreaterThen(Readings.Value(), Readings.Accuracy());
  }

  bool GreaterOrEqualTo(_Readings &Readings) {
    return GreaterOrEqualTo(Readings.Value(), Readings.Accuracy());
  }

  bool SmallerThen(_Readings &Readings) {
    return SmallerThen(Readings.Value(), Readings.Accuracy());
  }

  bool SmallerOrEqualTo(_Readings &Readings) {
    return SmallerOrEqualTo(Readings.Value(), Readings.Accuracy());
  }

  void setHysteresis(float Value = 0) {
    Stat.setHysteresis(Value);
  }

  float Hysteresis() {
    return Stat.Hysteresis();
  }

  void setStackSize(size_t Value = 0) {
    Stack.setSize(Value);
  }

  size_t StackSize() {
    return Stack.Size();
  }

  bool IsUp(ulong Period = 0) {
    return Stat.IsUp(Period);
  }

  bool IsDown(ulong Period = 0) {
    return Stat.IsDown(Period);
  }

  float IsUpBy() {
    return Stat.IsUpBy();
  }

  float IsDownBy() {
    return Stat.IsDownBy();
  }

  bool setStatEnabled(void *Iniciator, bool Value = false) {
    if (_Iniciator && (_Iniciator != Iniciator))
      return false;

    _Iniciator = Value ? Iniciator : nullptr;

    Stat.setEnabled(Value);

    return true;
  }

  bool StatEnabled() {
    return Stat.IsEnabled();
  }
};

enum class _SensorLocation : uint8_t {
  Undefined = 0,
  Inside,
  Outside
};

class _SensorList;

class _SensorCustom : public _ClassType, public _ClassOwner<_SensorList> {
private:
  _SensorLocation _Location = _SensorLocation::Undefined;

  bool _setActive(bool Value) {
    if (Value) {
      if (!_IsActive) {
        _StartMillis = millis();
        _IsActive    = true;
      }
    } else {
      if (_IsActive) {
        Reload();

        _StartMillis = 0;
        _IsActive    = false;
      }
    }
    return _IsActive;
  }

public:
  _SensorCustom(_SensorList &Sensors, ulong HeatingTime = 0, _SensorLocation Location = _SensorLocation::Undefined)
      : _ClassOwner<_SensorList>(Sensors) {
    setHeatingTime(HeatingTime);
    setLocation(Location);

    setMainType(_MainType::Sensor);
  }

  void Init() {
    OnInit();
  }

  void Update() {
    bool _result = getReadings();
    for (int i = 1; ((i < SENSOR_CHECK_COUNT) && !_result); i++) {
      _delay(SENSOR_CHECK_DELAY);

      _result = getReadings();
    }

    if (_result)
      _FailCount = 0;
    else if (++_FailCount < SENSOR_FAIL_COUNT)
      return;

    if (!_setActive(_result))
      return;

    OnUpdate();
  }

  void Reload() {
    OnReload();
  }

  bool IsValid() {
    return _IsActive;
  }

  bool IsHeating() {
    return (_IsActive && (_HeatingTime > 0) && ((millis() - _StartMillis) < _HeatingTime));
  }

  float HeatingPercent() {
    if (_IsActive) {
      if (_HeatingTime > 0)
        return ((millis() - _StartMillis) * (float)100) / _HeatingTime;
      else
        return (100);
    } else {
      return (0);
    }
  }

  void setHeatingTime(ulong Value = 0) {
    if (Value > 0)
      _HeatingTime = Value;
  }

  ulong HeatingTime() {
    return _HeatingTime;
  }

  void setLocation(_SensorLocation Location = _SensorLocation::Undefined) {
    _Location = Location;
  }

  _SensorLocation Location() {
    return _Location;
  }

  bool LocationIs(_SensorLocation Location = _SensorLocation::Undefined) {
    return _Location == Location;
  }

  ulong StartMillis() {
    return _StartMillis;
  }

  virtual _Readings *Readings(const _SubType subType) {
    return nullptr;
  }

protected:
  ulong _HeatingTime = 0;
  ulong _StartMillis = 0;
  uint8_t _FailCount = 0;

  bool _IsActive = false;

  virtual void OnInit() {
  }

  virtual void OnUpdate() {
  }

  virtual void OnReload() {
  }

  virtual bool getReadings() {
    return false;
  }
};

class _SensorDHT : public _SensorCustom {
protected:
  float _temperature = 0;
  float _humidity    = 0;

public:
  _Readings Temperature;
  _Readings Humidity;

  _SensorDHT(_SensorList &Sensors,
             size_t       TemperatureHistorySize = 0,
             size_t       HumidityHistorySize    = 0,
             size_t       TemperatureStackSize   = 0,
             size_t       HumidityStackSize      = 0)
      : _SensorCustom(Sensors),
        Temperature(*this,
                    TemperatureHistorySize,
                    TemperatureStackSize),
        Humidity(*this,
                 HumidityHistorySize,
                 HumidityStackSize) {
    Temperature.setSubType(_SubType::Temperature);
    Temperature.History.setSubType(_SubType::Temperature);
    Temperature.Stack.setSubType(_SubType::Temperature);
    Temperature.Stat.setSubType(_SubType::Temperature);

    Humidity.setSubType(_SubType::Humidity);
    Humidity.History.setSubType(_SubType::Humidity);
    Humidity.Stack.setSubType(_SubType::Humidity);
    Humidity.Stat.setSubType(_SubType::Humidity);

    Temperature.setPostfix("°C");
    Humidity.setPostfix("%");

    Temperature.OnGetText([this](char *Text) {
      sprintf(Text, "%s%.1f%s", Temperature.Prefix(), Temperature.Value(), Temperature.Postfix());
    });

    Humidity.OnGetText([this](char *Text) {
      sprintf(Text, "%s%.1f%s", Humidity.Prefix(), Humidity.Value(), Humidity.Postfix());
    });
  }

  virtual void OnUpdate() override {
    Temperature.putValue(_temperature);
    Humidity.putValue(_humidity);
  }

  virtual void OnReload() override {
    Temperature.Clear();
    Humidity.Clear();
  }

  virtual _Readings *Readings(const _SubType subType) override {
    if (Temperature.SubTypeIs(subType))
      return &Temperature;
    if (Humidity.SubTypeIs(subType))
      return &Humidity;

    return nullptr;
  }

  void setTemperatureAccuracy(float Value) {
    Temperature.setAccuracy(Value);

    debug.tprintf("%s.setTemperatureAccuracy(%.4f)\n", SubTypeName(), Value);
  }

  float TemperatureAccuracy() {
    return Temperature.Accuracy();
  }

  void setHumidityAccuracy(float Value) {
    Humidity.setAccuracy(Value);

    debug.tprintf("%s.setHumidityAccuracy(%.4f)\n", SubTypeName(), Value);
  }

  float HumidityAccuracy() {
    return Humidity.Accuracy();
  }

  void setTemperatureOffset(float Value) {
    Temperature.setOffset(Value);

    debug.tprintf("%s.setTemperatureOffset(%.4f)\n", SubTypeName(), Value);
  }

  float TemperatureOffset() {
    return Temperature.Offset();
  }

  void setHumidityOffset(float Value) {
    Humidity.setOffset(Value);

    debug.tprintf("%s.setHumidityOffset(%.4f)\n", SubTypeName(), Value);
  }

  float HumidityOffset() {
    return Humidity.Offset();
  }

  void setTemperatureHysteresis(float Value) {
    Temperature.setHysteresis(Value);

    debug.tprintf("%s.setTemperatureHysteresis(%.4f)\n", SubTypeName(), Value);
  }

  float TemperatureHysteresis() {
    return Temperature.Hysteresis();
  }

  void setHumidityHysteresis(float Value) {
    Humidity.setHysteresis(Value);

    debug.tprintf("%s.setHumidityHysteresis(%.4f)\n", SubTypeName(), Value);
  }

  float HumidityHysteresis() {
    return Humidity.Hysteresis();
  }

  void setTemperatureStackSize(size_t Value) {
    Temperature.setStackSize(Value);

    debug.tprintf("%s.setTemperatureStackSize(%d)\n", SubTypeName(), Value);
  }

  size_t TemperatureStackSize() {
    return Temperature.StackSize();
  }

  void setHumidityStackSize(size_t Value) {
    Humidity.setStackSize(Value);

    debug.tprintf("%s.setHumidityStackSize(%d)\n", SubTypeName(), Value);
  }

  size_t HumidityStackSize() {
    return Humidity.StackSize();
  }
};

class _SensorAM2320 : public _SensorDHT {
private:
  DHT _am2320;

public:
  _SensorAM2320(_SensorList &Sensors,
                int          Pin,
                size_t       TemperatureHistorySize = 0,
                size_t       HumidityHistorySize    = 0,
                size_t       TemperatureStackSize   = 0,
                size_t       HumidityStackSize      = 0)
      : _SensorDHT(Sensors,
                   TemperatureHistorySize,
                   HumidityHistorySize,
                   TemperatureStackSize,
                   HumidityStackSize),
        _am2320(Pin, DHT_TYPE) {
    _am2320.begin();

    Temperature.setAccuracy(AM2320_TEMPERATURE_ACCURACY);
    Humidity.setAccuracy(AM2320_HUMIDITY_ACCURACY);
  }

protected:
  virtual bool getReadings() override {
    _temperature = _am2320.readTemperature();
    _humidity    = _am2320.readHumidity();

    return (!isnan(_temperature) && !isnan(_humidity));
  }
};

class _SensorMHZ19 : public _SensorCustom {
private:
  MHZ19          _mhz19;
  HardwareSerial _Serial;

public:
  _Readings CO2;

  bool AutoCalibration = false;
  int  Range           = 0;

  _SensorMHZ19(_SensorList &Sensors,
               int          uart,
               int          rxPin,
               int          txPin,
               ulong        HeatingTime = 0,
               size_t       HistorySize = 0,
               size_t       StackSize   = 0)
      : _SensorCustom(Sensors,
                      HeatingTime),
        CO2(*this,
            HistorySize,
            StackSize),
        _Serial(uart) {
    CO2.setSubType(_SubType::CO2);
    CO2.History.setSubType(_SubType::CO2);
    CO2.Stack.setSubType(_SubType::CO2);
    CO2.Stat.setSubType(_SubType::CO2);

    CO2.setPostfix("ppm");

    _Serial.begin(9600, SERIAL_8N1, rxPin, txPin);
    _mhz19.begin(_Serial);

    AutoCalibration = _mhz19.getABC();
    Range           = _mhz19.getRange();

    CO2.setAccuracy(MHZ19_CO2_ACCURACY);
    CO2.OnGetAccuracy([this](float Accuracy) { return Accuracy + (0.05f * CO2.Value()); });

    CO2.OnGetText([this](char *Text) {
      if (IsHeating()) {
        sprintf(Text, "♨ %02d %%", (int)round(HeatingPercent()));

        return;
      }

      sprintf(Text, "%s%d%s", CO2.Prefix(), (int)round(CO2.Value()), CO2.Postfix());
    });
  }

  void setAutoCalibration(bool Value) {
    if (!_IsActive)
      return;

    _mhz19.autoCalibration(Value);
    _delay(50);

    AutoCalibration = _mhz19.getABC();

    debug.tprintf("%s.setAutoCalibration(%s)\n", SubTypeName(), Value ? "true" : "false");
  }

  void setRange(int Value) {
    if (!_IsActive || ((Value != 2000) && (Value != 5000)))
      return;

    _mhz19.setRange(Value);
    _delay(50);

    Range = _mhz19.getRange();

    debug.tprintf("%s.setRange(%d)\n", SubTypeName(), Range);
  }

  void Calibrate400() {
    if (!_IsActive)
      return;

    if (AutoCalibration) {
      setAutoCalibration(false);

      if (!AutoCalibration)
        _mhz19.calibrate();

      setAutoCalibration(true);
    } else {
      _mhz19.calibrate();
    }

    debug.tprintf("%s.Calibrate400()\n", SubTypeName());
  }

  void setCO2Accuracy(float Value) {
    CO2.setAccuracy(Value);

    debug.tprintf("%s.setCO2Accuracy(%.4f)\n", SubTypeName(), Value);
  }

  float CO2Accuracy() {
    return CO2.Accuracy();
  }

  void setCO2Offset(float Value) {
    CO2.setOffset(Value);

    debug.tprintf("%s.setCO2Offset(%.4f)\n", SubTypeName(), Value);
  }

  float CO2Offset() {
    return CO2.Offset();
  }

  void setCO2Hysteresis(float Value) {
    CO2.setHysteresis(Value);

    debug.tprintf("%s.setCO2Hysteresis(%.4f)\n", SubTypeName(), Value);
  }

  float CO2Hysteresis() {
    return CO2.Hysteresis();
  }

  void setCO2StackSize(size_t Value) {
    CO2.setStackSize(Value);

    debug.tprintf("%s.setCO2StackSize(%d)\n", SubTypeName(), Value);
  }

  size_t CO2StackSize() {
    return CO2.StackSize();
  }

  virtual _Readings *Readings(const _SubType subType) override {
    if (CO2.SubTypeIs(subType))
      return &CO2;

    return nullptr;
  }

protected:
  float _co2 = 0;

  virtual bool getReadings() override {
    _co2 = (float)_mhz19.getCO2();

    return (_co2 > 0);
  }

  void OnUpdate() override {
    if (IsHeating())
      return;

    CO2.putValue(_co2);
  }

  void OnReload() override {
    CO2.Clear();
  }
};

class _SensorList {
private:
  float _TemperatureOffsetSave = 0;
  float _HumidityOffsetSave    = 0;
  float _CO2OffsetSave         = 0;

public:
  _SensorAM2320 dht22in;
  _SensorAM2320 dht22out;
  _SensorMHZ19  mhz19in;
  _SensorMHZ19  mhz19out;

  _SensorList() : dht22in(*this, DHT_PIN),
                  dht22out(*this, DHT_EX_PIN),
                  mhz19in(*this, 1, CO2_RX_PIN, CO2_TX_PIN, MHZ19_HEATING_TIME),
                  mhz19out(*this, 2, CO2_EX_RX_PIN, CO2_EX_TX_PIN, MHZ19_HEATING_TIME) {
    dht22in.setLocation(_SensorLocation::Inside);
    dht22out.setLocation(_SensorLocation::Outside);
    mhz19in.setLocation(_SensorLocation::Inside);
    mhz19out.setLocation(_SensorLocation::Outside);

    dht22in.setSubType(_SubType::AM2320in);
    dht22out.setSubType(_SubType::AM2320out);
    mhz19in.setSubType(_SubType::MHZ19in);
    mhz19out.setSubType(_SubType::MHZ19out);
  }

  void Update() {
    dht22in.Update();
    dht22out.Update();
    mhz19in.Update();
    mhz19out.Update();
  }

  _SensorCustom *Sensors(_SubType subType, _SensorLocation Location = _SensorLocation::Undefined) {
    switch (subType) {
      case _SubType::Temperature:
      case _SubType::Humidity:
        switch (Location) {
          case _SensorLocation::Inside:
            return &dht22in;
          case _SensorLocation::Outside:
            return &dht22out;
        }
        break;

      case _SubType::CO2:
        switch (Location) {
          case _SensorLocation::Inside:
            return &mhz19in;
          case _SensorLocation::Outside:
            return &mhz19out;
        }
        break;
    }

    return nullptr;
  }

  // _Readings *Readings(_ClassType baseOwner, _ClassType baseType) {
  //   if (dht22in.Temperature.TypeIs(baseOwner, baseType))
  //     return &dht22in.Temperature;
  //   if (dht22in.Humidity.TypeIs(baseOwner, baseType))
  //     return &dht22in.Humidity;
  //   if (mhz19in.CO2.TypeIs(baseOwner, baseType))
  //     return &mhz19in.CO2;
  //   if (mhz19out.CO2.TypeIs(baseOwner, baseType))
  //     return &mhz19out.CO2;

  //   return nullptr;
  // }

  // _Readings *Readings(int baseOwnerID, int TypeID) {
  //   return Readings(static_cast<_ClassType>(baseOwnerID),
  //                   static_cast<_ClassType>(TypeID));
  // }

  // _ReadingsStat *Stat(_ClassType baseOwner, _ClassType baseType) {
  //   if (dht22in.Temperature.Stat.TypeIs(baseOwner, baseType))
  //     return &dht22in.Temperature.Stat;
  //   if (dht22in.Humidity.Stat.TypeIs(baseOwner, baseType))
  //     return &dht22in.Humidity.Stat;
  //   if (dht22out.Temperature.Stat.TypeIs(baseOwner, baseType))
  //     return &dht22out.Temperature.Stat;
  //   if (dht22out.Humidity.Stat.TypeIs(baseOwner, baseType))
  //     return &dht22out.Humidity.Stat;
  //   if (mhz19in.CO2.Stat.TypeIs(baseOwner, baseType))
  //     return &mhz19in.CO2.Stat;
  //   if (mhz19out.CO2.Stat.TypeIs(baseOwner, baseType))
  //     return &mhz19out.CO2.Stat;

  //   return nullptr;
  // }

  // _ReadingsStat *Stat(int baseOwnerID, int TypeID) {
  //   return Stat(static_cast<_ClassType>(baseOwnerID),
  //               static_cast<_ClassType>(TypeID));
  // }

  void setTemperatureStatEnabled(void *Iniciator, bool Value) {
    dht22in.Temperature.setStatEnabled(Iniciator, Value);
    dht22out.Temperature.setStatEnabled(Iniciator, Value);
  }

  void setHumidityStatEnabled(void *Iniciator, bool Value) {
    dht22in.Humidity.setStatEnabled(Iniciator, Value);
    dht22out.Humidity.setStatEnabled(Iniciator, Value);
  }

  void setCO2StatEnabled(void *Iniciator, bool Value) {
    mhz19in.CO2.setStatEnabled(Iniciator, Value);
    mhz19out.CO2.setStatEnabled(Iniciator, Value);
  }

  // void OnStatEnabledChange(_ReadingsStat::_OnEnabledChange cb) {
  //   if (!cb)
  //     return;

  //   dht22in.Temperature.Stat.OnEnabledChange(cb);
  //   dht22in.Humidity.Stat.OnEnabledChange(cb);
  //   dht22out.Temperature.Stat.OnEnabledChange(cb);
  //   dht22out.Humidity.Stat.OnEnabledChange(cb);
  //   mhz19in.CO2.Stat.OnEnabledChange(cb);
  //   mhz19out.CO2.Stat.OnEnabledChange(cb);
  // }

  void OnStatDirectionChange(_ReadingsStat::_OnDirectionChange cb) {
    if (!cb)
      return;

    dht22in.Temperature.Stat.OnDirectionChange(cb);
    dht22in.Humidity.Stat.OnDirectionChange(cb);
    dht22out.Temperature.Stat.OnDirectionChange(cb);
    dht22out.Humidity.Stat.OnDirectionChange(cb);
    mhz19in.CO2.Stat.OnDirectionChange(cb);
    mhz19out.CO2.Stat.OnDirectionChange(cb);
  }

  void OnStatValueChange(_ReadingsStat::_OnValueChange cb) {
    if (!cb)
      return;

    dht22in.Temperature.Stat.OnValueChange(cb);
    dht22in.Humidity.Stat.OnValueChange(cb);
    dht22out.Temperature.Stat.OnValueChange(cb);
    dht22out.Humidity.Stat.OnValueChange(cb);
    mhz19in.CO2.Stat.OnValueChange(cb);
    mhz19out.CO2.Stat.OnValueChange(cb);
  }

  void OnStatValueAdd(_ReadingsStat::_OnValueAdd cb) {
    if (!cb)
      return;

    dht22in.Temperature.Stat.OnValueAdd(cb);
    dht22in.Humidity.Stat.OnValueAdd(cb);
    dht22out.Temperature.Stat.OnValueAdd(cb);
    dht22out.Humidity.Stat.OnValueAdd(cb);
    mhz19in.CO2.Stat.OnValueAdd(cb);
    mhz19out.CO2.Stat.OnValueAdd(cb);
  }
};

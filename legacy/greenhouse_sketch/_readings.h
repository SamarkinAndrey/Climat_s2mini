#pragma once

#include "_common.h"

using namespace std;

struct _IntervalData {
  ulong MillisFrom = 0;
  ulong MillisTo   = 0;

  float ValueFrom = 0;
  float ValueTo   = 0;

  float ValueMin = 0;
  float ValueMax = 0;
  float ValueAvg = 0;

  float ValueUp   = 0;
  float ValueDown = 0;

  float Vibration        = 0;
  float VibrationPercent = 0;

  int RecordCount = 0;

  char MillisFromStr[50] = "";
  char MillisToStr[50]   = "";
};

template <typename T>
struct _HistoryItem {
  static_assert(is_arithmetic<T>::value, "_HistoryItem: type <T> must be numeric.");

  T     Value  = T();
  ulong Millis = 0;

  _HistoryItem(T _Value = T(), ulong _Millis = 0)
      : Value(_Value), Millis(_Millis) {
  }
};

template <typename T>
class _ReadingsHistory : public _UsingName, public _UsingIniciator {
  static_assert(is_arithmetic<T>::value, "_ReadingsHistory: type <T> must be numeric.");

private:
  _HistoryItem<T> *_history = nullptr;

  size_t _size_max = READINGS_HISTORY_SIZE;
  size_t _size     = 0;
  size_t _start    = 0;
  size_t _end      = 0;

  void _setSizeMax(size_t Value = 0) {
    if ((Value > 0) && (Value <= READINGS_HISTORY_SIZE))
      _size_max = Value;
  }

  void _Destroy() {
    if (!_history)
      return;

    delete[] _history;

    _history = nullptr;
  }

public:
  ulong MillisFrom = 0;
  ulong MillisTo   = 0;

  char MillisFromStr[50] = "";
  char MillisToStr[50]   = "";
  
  _ReadingsHistory(size_t size_max = READINGS_HISTORY_SIZE, bool Enabled = false) {
    _setSizeMax(size_max);

    setEnabled(nullptr, Enabled);
  }

  ~_ReadingsHistory() {
    _Destroy();
  }

  void setEnabled(void *Iniciator, bool Value) {
    if (Value == getEnabled() || (!Value && (_Iniciator != Iniciator)))
      return;

    _Destroy();

    if (Value) {
      _history = new _HistoryItem<T>[_size_max];
    } else {
      Clear();
    }

    _Iniciator = Iniciator;

    debug.tprintf("%s.setEnabled(%s)\n", _Name, Value ? "true" : "false");
  }

  bool getEnabled() {
    return (_history != nullptr);
  }

  void putValue(T Value = T()) {
    if (!getEnabled() || (_size_max == 0) || (is_floating_point<T>::value && isnan(Value)))
      return;

    ulong _millis = millis();

    _history[_end] = {Value, _millis};
    _end           = (_end + 1) % _size_max;

    if (_size < _size_max) {
      _size++;
    } else {
      _start = (_start + 1) % _size_max;
    }

    MillisFrom = _history[_start].Millis;
    MillisTo   = _history[(_end - 1 + _size_max) % _size_max].Millis;

    MillisToTimeStr24(MillisFromStr, MillisFrom);
    MillisToTimeStr24(MillisToStr, MillisTo);
  }

  _IntervalData getIntervalStat(ulong IntervalSec = 0) {
    _IntervalData _Stat = {};

    if (!getEnabled() || (IntervalSec < 1) || (_size == 0))
      return _Stat;

    ulong _millis   = millis();
    ulong _interval = IntervalSec * 1000UL;

    ulong _millis_to   = 0;
    ulong _millis_from = 0;

    T _value_min  = numeric_limits<T>::max();
    T _value_max  = numeric_limits<T>::lowest();
    T _value_from = T();
    T _value_to   = T();

    T _sum = T();

    int _count = 0;

    int _pos  = (_end - 1 + _size_max) % _size_max;
    int _from = 0;
    int _to   = 0;

    int _checked = 0;

    while (_checked < _size && (_millis - _history[_pos].Millis) <= _interval) {
      T _value = _history[_pos].Value;

      if (!is_floating_point<T>::value || !isnan(_value)) {
        _from = _pos;
        if (_count < 1)
          _to = _from;

        _sum += _value;

        _value_min = min(_value_min, _value);
        _value_max = max(_value_max, _value);

        _count++;
      }

      _pos = (_pos - 1 + _size_max) % _size_max;

      _checked++;
    }

    if (_count < 1)
      return _Stat;

    _millis_from = _history[_from].Millis;
    _millis_to   = _history[_to].Millis;

    _value_from = _history[_from].Value;
    _value_to   = _history[_to].Value;

    _Stat.MillisFrom = _millis_from;
    _Stat.MillisTo   = _millis_to;
    _Stat.ValueFrom  = _value_from;
    _Stat.ValueTo    = _value_to;
    _Stat.ValueMin   = _value_min;
    _Stat.ValueMax   = _value_max;
    _Stat.ValueAvg   = (_count > 0) ? _sum / _count : T();

    _Stat.ValueUp          = (_value_to > _value_from) ? _value_to - _value_from : 0;
    _Stat.ValueDown        = (_value_from > _value_to) ? _value_from - _value_to : 0;
    _Stat.Vibration        = _value_max - _value_min;
    _Stat.VibrationPercent = (_value_max != 0) ? ((_value_max - _value_min) * (100.0 / _value_max)) : 0;

    _Stat.RecordCount = _count;

    MillisToTimeStr24(_Stat.MillisFromStr, _Stat.MillisFrom);
    MillisToTimeStr24(_Stat.MillisToStr, _Stat.MillisTo);

    return _Stat;
  }

  void Clear() {
    _start = 0;
    _end   = 0;
    _size  = 0;

    MillisFrom = 0;
    MillisTo   = 0;

    MillisFromStr[0] = '\0';
    MillisToStr[0]   = '\0';
  }

  size_t getSize() {
    return _size;
  }

  size_t getSizeMax() {
    return _size_max;
  }
};

template <typename T>
class _ReadingsStack : public _UsingName {
  static_assert(is_arithmetic<T>::value, "_ReadingsStack: type <T> must be numeric.");

private:
  T     *_stack    = nullptr;
  size_t _size_max = READINGS_STACK_SIZE;
  size_t _size     = 0;
  size_t _start    = 0;
  T      _sum      = T();

  void _resize(size_t size_max = 0) {
    if ((size_max < 1) || ((size_max == _size_max) && _stack))
      return;

    T *new_stack = new T[size_max];
    if (!new_stack)
      return;

    size_t new_size = min(_size, size_max);
    T      new_sum  = T();

    for (size_t i = 0; i < new_size; i++) {
      new_stack[i] = _stack[(_start + i) % _size_max];
      new_sum += new_stack[i];
    }

    if (_stack)
      delete[] _stack;

    _stack    = new_stack;
    _size_max = size_max;
    _start    = 0;
    _size     = new_size;
    _sum      = new_sum;

    Value = (_size > 0) ? _sum / static_cast<T>(_size) : T();
  }

public:
  T Value = T();

  _ReadingsStack(size_t size_max = READINGS_STACK_SIZE) {
    setSize(size_max);
  }

  ~_ReadingsStack() {
    if (_stack)
      delete[] _stack;
  }
  void clear() {
    _start = 0;
    _size  = 0;
    _sum   = T();

    Value = T();
  }

  bool empty() {
    return (_size == 0);
  }

  size_t size() {
    return _size;
  }

  T putValue(T value = T()) {
    if (is_floating_point<T>::value && isnan(value))
      return Value;

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

    Value = (_size > 0) ? _sum / static_cast<T>(_size) : T();

    return Value;
  }

  void setSize(size_t size = 0) {
    _resize(size);
  }

  size_t getSize() {
    return _size_max;
  }
};

class _Readings {
private:
  float _Offset = 0;

public:
  _ReadingsStack<float>   Stack;
  _ReadingsHistory<float> History;

  float Value    = 0;
  char  Text[50] = "";

  _Readings(int    HistorySize = 0,
            size_t StackSize  = 0)
      : History(HistorySize),
        Stack(StackSize) {
  }

  void Clear() {
    Value   = 0;
    Text[0] = '\0';

    Stack.clear();
  }

  void putValue(float _Value = 0) {
    Value = Stack.putValue(_Value + _Offset);

    History.putValue(Value);
  }

  float getValue() {
    return Stack.Value;
  }

  void setOffset(float Value = 0) {
    _Offset = Value;
  }

  float getOffset() {
    return _Offset;
  }

  void setStackSize(size_t Value = 0) {
    Stack.setSize(Value);
  }

  size_t getStackSize() {
    return Stack.getSize();
  }
};

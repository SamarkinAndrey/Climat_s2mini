#pragma once

#include <list>

#include "_common.h"

using PL = size_t[];

template <typename T>
class _ParamState {
private:
  struct _Param {
    size_t _ID        = 0;
    T      _Value     = T();
    bool   _IsChanged = false;

    _Param(size_t id, T value, bool isChanged)
        : _ID(id), _Value(value), _IsChanged(isChanged) {}
  };

  std::list<_Param> _ParamList;

public:
  class Proxy {
  private:
    _ParamState &_state;
    size_t       _ID;

  public:
    Proxy(_ParamState &state, size_t ID) : _state(state), _ID(ID) {}

    void operator=(const T &value) {
      _state.setValue(_ID, value);
    }

    operator T() const {
      return _state.getValue(_ID);
    }
  };

  Proxy operator[](size_t ID) {
    return Proxy(*this, ID);
  }

  void setValue(size_t ID, T Value) {
    for (auto &p : _ParamList) {
      if (p._ID == ID) {
        if (p._Value != Value) {
          p._Value     = Value;
          p._IsChanged = true;
        } else {
          p._IsChanged = false;
        }
        return;
      }
    }
    _ParamList.push_back({ID, Value, false});
  }

  T getValue(size_t ID) {
    for (const auto &p : _ParamList) {
      if (p._ID == ID) {
        return p._Value;
      }
    }
    return T();
  }

  bool IsChanged(size_t IDList[] = nullptr, int Size = 0) {
    for (const auto &p : _ParamList) {
      if (IDList && (Size > 0)) {
        for (int a = 0; a < Size; a++) {
          if ((p._ID == IDList[a]) && (p._IsChanged))
            return true;
        }
      } else {
        if (p._IsChanged)
          return true;
      }
    }
    return false;
  }
};

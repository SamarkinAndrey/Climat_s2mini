#pragma once

#include <Arduino.h>

enum class _MainType : uint8_t {
  Undefined = 0,
  Sensor,
  Device,
  Alert,
  Readings,
  Stack,
  Stat,
  History,
  Sync,
  Plot
};

const char *_MainTypeName[] PROGMEM = {
    "Undefined",
    "Sensor",
    "Device",
    "Alert",
    "Readings",
    "Stack",
    "Stat",
    "History",
    "Sync",
    "Plot"};

enum class _SubType : uint8_t {
  Undefined = 0,
  DHT22in,
  DHT22out,
  AM2320in,
  AM2320out,
  MHZ19in,
  MHZ19out,
  FanMain,
  FanInner,
  Humidifier,
  Heater,
  Temperature,
  Humidity,
  CO2
};

const char *_SubTypeName[] PROGMEM = {
    "Undefined",
    "DHT22(inner)",
    "DHT22(outer)",
    "AM2320(inner)",
    "AM2320(outer)",
    "MHZ19(inner)",
    "MHZ19(outer)",
    "Fan(main)",
    "Fan(inner)",
    "Humidifier",
    "Heater",
    "Temperature",
    "Humidity",
    "CO2"};

inline _MainType TypeIDToMainType(int TypeID) {
  return static_cast<_MainType>(TypeID >> 8);
}

inline _SubType TypeIDToSubType(int TypeID) {
  return static_cast<_SubType>(TypeID & 0xFF);
}

inline int MakeTypeID(_MainType mainType, _SubType subType) {
  return (static_cast<int>(mainType) << 8) | static_cast<int>(subType);
}

class _ClassType {
private:
  _MainType _mainType = _MainType::Undefined;
  _SubType  _subType  = _SubType::Undefined;

  int  _ID       = 0;
  char _Name[50] = "";

  void _makeID() {
    _ID = MakeTypeID(_mainType, _subType);
  }

  void _makeName() {
    snprintf(_Name, sizeof(_Name), "%s.%s", MainTypeName(), SubTypeName());
  }

public:
  _ClassType(_MainType Type    = _MainType::Undefined,
             _SubType  SubType = _SubType::Undefined)
      : _mainType(Type), _subType(SubType) {
    _makeID();
    _makeName();
  }

  _ClassType(int TypeID) {
    setID(TypeID);
  }

  bool operator==(const _MainType &mainType) const {
    return MainTypeIs(mainType);
  }

  bool operator!=(const _MainType &mainType) const {
    return !MainTypeIs(mainType);
  }

  bool operator==(const _SubType &subType) const {
    return SubTypeIs(subType);
  }

  bool operator!=(const _SubType &subType) const {
    return !SubTypeIs(subType);
  }

  bool operator==(const _ClassType &Type) const {
    return TypeIs(Type);
  }

  bool operator!=(const _ClassType &Type) const {
    return !TypeIs(Type);
  }

  bool operator==(const int &TypeID) const {
    return TypeIs(TypeID);
  }

  bool operator!=(const int &TypeID) const {
    return !TypeIs(TypeID);
  }

  void setType(_ClassType Type) {
    _mainType = Type._mainType;
    _subType  = Type._subType;
    _ID       = Type._ID;
    _makeName();
  }

  void setType(_MainType mainType, _SubType subType) {
    _mainType = mainType;
    _subType  = subType;
    _makeID();
    _makeName();
  }

  _ClassType Type() {
    return *this;
  }

  const _ClassType Type() const {
    return *this;
  }

  void setID(int TypeID) {
    _mainType = TypeIDToMainType(TypeID);
    _subType  = TypeIDToSubType(TypeID);
    _ID       = TypeID;
    _makeName();
  }

  int ID() {
    return _ID;
  }

  const int ID() const {
    return _ID;
  }

  void setMainType(_MainType mainType) {
    _mainType = mainType;
    _makeID();
    _makeName();
  }

  _MainType &MainType() {
    return _mainType;
  }

  const _MainType &MainType() const {
    return _mainType;
  }

  void setSubType(_SubType subType) {
    _subType = subType;
    _makeID();
    _makeName();
  }

  _SubType &SubType() {
    return _subType;
  }

  const _SubType &SubType() const {
    return _subType;
  }

  bool MainTypeIs(_MainType mainType) {
    return (_mainType == mainType);
  }

  bool MainTypeIs(_MainType mainType) const {
    return (_mainType == mainType);
  }

  bool MainTypeIs(int TypeID) {
    return (_mainType == TypeIDToMainType(TypeID));
  }

  bool MainTypeIs(int TypeID) const {
    return (_mainType == TypeIDToMainType(TypeID));
  }

  bool SubTypeIs(_SubType subType) {
    return (_subType == subType);
  }

  bool SubTypeIs(_SubType subType) const {
    return (_subType == subType);
  }

  bool SubTypeIs(int TypeID) {
    return (_subType == TypeIDToSubType(TypeID));
  }

  bool SubTypeIs(int TypeID) const {
    return (_subType == TypeIDToSubType(TypeID));
  }

  bool TypeIs(_MainType mainType, _SubType subType) {
    return (MainTypeIs(mainType) && SubTypeIs(subType));
  }

  bool TypeIs(_MainType mainType, _SubType subType) const {
    return (MainTypeIs(mainType) && SubTypeIs(subType));
  }

  bool TypeIs(_ClassType Type) {
    return TypeIs(Type._mainType, Type._subType);
  }

  bool TypeIs(_ClassType Type) const {
    return TypeIs(Type._mainType, Type._subType);
  }

  bool TypeIs(int TypeID) {
    return TypeIs(TypeIDToMainType(TypeID), TypeIDToSubType(TypeID));
  }

  bool TypeIs(int TypeID) const {
    return TypeIs(TypeIDToMainType(TypeID), TypeIDToSubType(TypeID));
  }

  const char *Name() {
    return _Name;
  }

  const char *MainTypeName() {
    return _MainTypeName[static_cast<uint8_t>(_mainType)];
  }

  const char *SubTypeName() {
    return _SubTypeName[static_cast<uint8_t>(_subType)];
  }
};

template <typename T>
class _ClassOwner {
protected:
  T &_Owner;

public:
  explicit _ClassOwner(T &owner) : _Owner(owner) {
  }

  _ClassOwner(const _ClassOwner &)            = delete;
  _ClassOwner &operator=(const _ClassOwner &) = delete;

  const T &Owner() const {
    return _Owner;
  }

  T &Owner() {
    return _Owner;
  }

  operator T &() {
    return _Owner;
  }

  operator const T &() const {
    return _Owner;
  }
};

#pragma once

#include <Arduino.h>

inline void _millis_to_str(char _buffer[], unsigned long _millis = 0) {
  uint32_t sec = _millis / 1000ul;
  uint32_t ms  = _millis % 1000ul;

  int hours = (sec / 3600ul) % 24;
  int mins  = (sec % 3600ul) / 60ul;
  int secs  = (sec % 3600ul) % 60ul;

  sprintf(_buffer, "%02d:%02d:%02d:%03d", hours, mins, secs, ms);
}

enum _DebugMode {
  DM_DISABLED,
  DM_SERIAL,
  DM_LOGGER,
  DM_ALL
};

class _Debug : public Print {
private:
  Print &_serial;
  Print &_logger;

  _DebugMode _mode = DM_DISABLED;

  size_t _print_time(unsigned long _millis = 0) {
    unsigned long _mil;

    if (_millis > 0)
      _mil = _millis;
    else
      _mil = millis();

    return print_time(_mil) + print(" ");
  }

public:
  _Debug(Print &serial, Print &logger) : _serial(serial), _logger(logger) {}

  void setMode(_DebugMode Value) {
    _mode = Value;
  }

  void setModeInt(uint8_t Value) {
    switch (Value) {
      case 1:
        _mode = DM_SERIAL;
        break;
      case 2:
        _mode = DM_LOGGER;
        break;
      case 3:
        _mode = DM_ALL;
        break;
      default:
        _mode = DM_DISABLED;
    }
  }

  _DebugMode getMode() {
    return _mode;
  }

  uint8_t getModeInt() {
    switch (_mode) {
      case DM_SERIAL:
        return 1;
      case DM_LOGGER:
        return 2;
      case DM_ALL:
        return 3;
      default:
        return 0;
    }
  }

  size_t write(uint8_t data) override {
    switch (_mode) {
      case DM_ALL:
        return _serial.write(data) + _logger.write(data);
      case DM_SERIAL:
        return _serial.write(data);
      case DM_LOGGER:
        return _logger.write(data);
      case DM_DISABLED:
      default:
        return 0;
    }
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    switch (_mode) {
      case DM_ALL:
        return _serial.write(buffer, size) + _logger.write(buffer, size);
      case DM_SERIAL:
        return _serial.write(buffer, size);
      case DM_LOGGER:
        return _logger.write(buffer, size);
      case DM_DISABLED:
      default:
        return 0;
    }
  }

  size_t tprintf(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    if (_mode == DM_DISABLED)
      return 0;

    size_t  result = _print_time();
    char    buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    result += print(buffer);
    return result;
  }

  size_t tprint(const __FlashStringHelper *ifsh) {
    return _print_time() + print(reinterpret_cast<const char *>(ifsh));
  }

  size_t tprint(const String &s) {
    return _print_time() + print(s);
  }

  size_t tprint(const char str[]) {
    return _print_time() + print(str);
  }

  size_t tprint(char c) {
    return _print_time() + print(c);
  }

  size_t tprint(unsigned char c, int base = DEC) {
    return _print_time() + print(c, base);
  }

  size_t tprint(int num, int base = DEC) {
    return _print_time() + print(num, base);
  }

  size_t tprint(unsigned int num, int base = DEC) {
    return _print_time() + print(num, base);
  }

  size_t tprint(long num, int base = DEC) {
    return _print_time() + print(num, base);
  }

  size_t tprint(unsigned long num, int base = DEC) {
    return _print_time() + print(num, base);
  }

  size_t tprint(long long num, int base = DEC) {
    return _print_time() + print((long)num, base);
  }

  size_t tprint(unsigned long long num, int base = DEC) {
    return _print_time() + print((unsigned long)num, base);
  }

  size_t tprint(double num, int digits = 2) {
    return _print_time() + print(num, digits);
  }

  size_t tprint(const Printable &p) {
    return _print_time() + print(p);
  }

  size_t tprint(struct tm *timeinfo, const char *format = NULL) {
    size_t result = _print_time();
    if (format) {
      char buffer[64];
      strftime(buffer, sizeof(buffer), format, timeinfo);
      result += print(buffer);
    } else {
      result += print(timeinfo->tm_year + 1900);
      result += print("-");
      result += print(timeinfo->tm_mon + 1);
      result += print("-");
      result += print(timeinfo->tm_mday);
      result += print(" ");
      result += print(timeinfo->tm_hour);
      result += print(":");
      result += print(timeinfo->tm_min);
      result += print(":");
      result += print(timeinfo->tm_sec);
    }
    return result;
  }

  size_t tprintln(const __FlashStringHelper *ifsh) {
    return _print_time() + println(reinterpret_cast<const char *>(ifsh));
  }

  size_t tprintln(const String &s) {
    return _print_time() + println(s);
  }

  size_t tprintln(const char str[]) {
    return _print_time() + println(str);
  }

  size_t tprintln(char c) {
    return _print_time() + println(c);
  }

  size_t tprintln(unsigned char c, int base = DEC) {
    return _print_time() + println(c, base);
  }

  size_t tprintln(int num, int base = DEC) {
    return _print_time() + println(num, base);
  }

  size_t tprintln(unsigned int num, int base = DEC) {
    return _print_time() + println(num, base);
  }

  size_t tprintln(long num, int base = DEC) {
    return _print_time() + println(num, base);
  }

  size_t tprintln(unsigned long num, int base = DEC) {
    return _print_time() + println(num, base);
  }

  size_t tprintln(long long num, int base = DEC) {
    return _print_time() + println((long)num, base);
  }

  size_t tprintln(unsigned long long num, int base = DEC) {
    return _print_time() + println((unsigned long)num, base);
  }

  size_t tprintln(double num, int digits = 2) {
    return _print_time() + println(num, digits);
  }

  size_t tprintln(const Printable &p) {
    return _print_time() + println(p);
  }

  size_t tprintln(struct tm *timeinfo, const char *format = NULL) {
    size_t result = _print_time();
    if (format) {
      char buffer[64];
      strftime(buffer, sizeof(buffer), format, timeinfo);
      result += println(buffer);
    } else {
      result += print(timeinfo->tm_year + 1900);
      result += print("-");
      result += print(timeinfo->tm_mon + 1);
      result += print("-");
      result += print(timeinfo->tm_mday);
      result += print(" ");
      result += print(timeinfo->tm_hour);
      result += print(":");
      result += print(timeinfo->tm_min);
      result += print(":");
      result += print(timeinfo->tm_sec);
      result += println();
    }
    return result;
  }

  size_t tprintln() {
    return _print_time() + println();
  }

  size_t print_time(unsigned long _millis = 0) {
    char buffer[25] = "";

    _millis_to_str(buffer, _millis);

    return print("[") + print(buffer) + print("]");
  }

  size_t print_timeln(unsigned long _millis = 0) {
    return print_time(_millis) + println();
  }
};

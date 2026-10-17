#pragma once

#include "_common.h"
#include <Table.h>
#include <vector>

#define HIMIDITY_PLOT_SIZE          500
#define HIMIDITY_PLOT_EPSILON       0.01f
#define HIMIDITY_PLOT_SAVE_INTERVAL 60ul * 1000ul

class _Plot : public _ClassType {
private:
  Table _data;
  Table _point;
  Table _temp;

  size_t _maxSize      = HIMIDITY_PLOT_SIZE;
  float  _epsilon      = HIMIDITY_PLOT_EPSILON;
  ulong  _saveInterval = HIMIDITY_PLOT_SAVE_INTERVAL;
  ulong  _saveMillis   = 0;

  float _lastValue = NAN;
  bool  _changed   = false;

  static const size_t RDP_STACK_SIZE = 128;

  char file_name[50] = "";

  bool _loaded = false;

  inline float perpendicularDistance(const uint64_t &time1, float value1,
                                     const uint64_t &time2, float value2,
                                     const uint64_t &timeP, float valueP) const {
    const float dx = static_cast<float>(time2 - time1);
    if (dx == 0)
      return fabs(valueP - value1);

    const float slope = (value2 - value1) / dx;
    const float tP    = static_cast<float>(timeP - time1);

    return fabs(slope * tP + (value1 - valueP)) / sqrt(slope * slope + 1.0f);
  }

  void rdpCompressImpl(Table &src, std::vector<bool> &keep, float epsilon) {
    struct Segment {
      size_t start, end;

      Segment(size_t s, size_t e) : start(s), end(e) {}
    };

    if (src.rows() < 2)
      return;

    std::vector<Segment> stack;
    stack.reserve(RDP_STACK_SIZE);
    stack.emplace_back(0, src.rows() - 1);

    while (!stack.empty()) {
      auto segment = stack.back();
      stack.pop_back();

      if (segment.end <= segment.start + 1)
        continue;

      uint64_t timeStart  = src[segment.start][0];
      float    valueStart = src[segment.start][1];
      uint64_t timeEnd    = src[segment.end][0];
      float    valueEnd   = src[segment.end][1];

      float  maxDist  = 0;
      size_t maxIndex = segment.start;

      for (size_t i = segment.start + 1; i < segment.end; ++i) {
        uint64_t timeP  = src[i][0];
        float    valueP = src[i][1];
        float    dist   = perpendicularDistance(timeStart, valueStart, timeEnd, valueEnd, timeP, valueP);
        if (dist > maxDist) {
          maxDist  = dist;
          maxIndex = i;
        }
      }

      if (maxDist > epsilon) {
        keep[maxIndex] = true;
        stack.emplace_back(segment.start, maxIndex);
        stack.emplace_back(maxIndex, segment.end);
      }
    }
  }

  void cutData(Table &data) {
    while (data.rows() > _maxSize)
      data.remove(0);
  }

  void makeFileName() {
    snprintf(file_name, sizeof(file_name), "/%s_plot.tbl", SubTypeName());
    toLowerCase(file_name);
  }

public:
  _Plot(_SubType subType, size_t maxSize = HIMIDITY_PLOT_SIZE, float epsilon = HIMIDITY_PLOT_EPSILON) {
    _data.init(2, cell_t::Uint64, cell_t::Float);
    _point.init(2, cell_t::Uint64, cell_t::Float);
    _temp.init(2, cell_t::Uint64, cell_t::Float);

    _data.clearChanged();

    setMaxSize(maxSize);
    setEpsilon(epsilon);

    setMainType(_MainType::Plot);
    setSubType(subType);

    makeFileName();
  }

  void Compress() {
    if ((_epsilon <= 0) || (_temp.rows() <= 2) || !_temp.changed())
      return;

    std::vector<bool> keep(_temp.rows(), false);

    rdpCompressImpl(_temp, keep, _epsilon);

    keep.front() = _data.rows() < 1;
    keep.back()  = true;

    for (size_t i = 0; i < _temp.rows(); ++i) {
      if (keep[i]) {
        _data.append(_temp[i][0].toInt64(),
                     _temp[i][1].toFloat());
      }
    }

    cutData(_data);

    _temp.removeAll();

    tbl::Row r = _data.get(-1);

    _temp.append(r[0].toInt64(), r[1].toFloat());
    _temp.clearChanged();
  }

  void putValue(float value) {
    if (!std::isfinite(value) || !sett.rtc.synced())
      return;

    if (value == _lastValue)
      return;

    _lastValue = value;
    _changed   = true;

    uint64_t _unixMs = sett.rtc.getUnixMs();

    _point.append(_unixMs, value);

    cutData(_point);

    if (_maxSize < 1)
      return;

    _temp.append(_unixMs, value);

    if (_temp.rows() > _maxSize)
      Compress();
  }

  bool Save() {
    if (!_FS_Initialized || !file_name || !*file_name || (_data.rows() < 1 && _temp.rows() < 1))
      return false;

    File f = LittleFS.open(file_name, FILE_WRITE);
    if (!f)
      return false;

    Compress();

    return _data.writeTo(f);
  }

  bool Load() {
    if (!_FS_Initialized || !file_name || !*file_name || _maxSize < 1)
      return false;

    File f = LittleFS.open(file_name, FILE_READ);
    if (!f)
      return false;

    if (!_data.readFrom(f, f.size()))
      return false;

    Compress();

    return true;
  }

  void Tick() {
    if (!_FS_Initialized || (_maxSize < 1) || (_saveInterval < 1))
      return;

    if ((_saveMillis > 0) && ((millis() - _saveMillis) < _saveInterval))
      return;

    _saveMillis = millis();

    if (!_loaded) {
      Load();

      _loaded = true;
    } else if (_changed) {
      _changed = false;

      Save();
    }
  }

  void Clear() {
    _data.removeAll();
    _temp.removeAll();

    _lastValue = NAN;
    _changed   = false;
    _saveMillis = millis();
  }

  size_t MaxSize() const {
    return _maxSize;
  }

  float Epsilon() const {
    return _epsilon;
  }

  ulong SaveInterval() const {
    return _saveInterval;
  }

  Table &Data() {
    Compress();

    return _data;
  }

  Table &Point() {
    return _point;
  }

  size_t CurrentSize() {
    return _data.rows();
  }

  void setMaxSize(size_t size) {
    _maxSize = size;

    if (_maxSize < 1) {
      Clear();

      return;
    }

    if (_data.rows() > _maxSize)
      cutData(_data);
  }

  void setEpsilon(float epsilon) {
    _epsilon = epsilon;
  }

  void setSaveInterval(ulong interval) {
    _saveInterval = interval;
  }
};

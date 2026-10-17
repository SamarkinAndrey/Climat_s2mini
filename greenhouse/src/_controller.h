#pragma once

#include "_common.h"
#include <Arduino.h>

#define STAT_RANGE_STEP  5
#define STAT_RANGE_COUNT 100 / STAT_RANGE_STEP
#define STAT_BUFFER_SIZE 10
#define STAT_PREDICT_MIN 10000ul
#define STAT_PREDICT_MAX 20000ul

class DeviceController : public _ClassType {
private:
  float efficiencyInc[STAT_RANGE_COUNT][STAT_BUFFER_SIZE] = {0};
  float efficiencyDec[STAT_RANGE_COUNT][STAT_BUFFER_SIZE] = {0};

  float avgEfficiencyInc[STAT_RANGE_COUNT] = {0};
  float avgEfficiencyDec[STAT_RANGE_COUNT] = {0};

  uint8_t bufferIndexInc[STAT_RANGE_COUNT] = {0};
  uint8_t bufferIndexDec[STAT_RANGE_COUNT] = {0};
  ulong   SaveMillis                       = 0;
  ulong   SaveMillisInterval               = 30000;

  bool _IsLoaded = false;

  bool _IsChanged = false;

  ulong _PredictMin = STAT_PREDICT_MIN;
  ulong _PredictMax = STAT_PREDICT_MAX;

  void save() {
    if (!_IsLoaded || !_FS_Initialized)
      return;

    char file_name[50];
    snprintf(file_name, sizeof(file_name), "/%s_stat.csv", SubTypeName());
    toLowerCase(file_name);

    File f = LittleFS.open(file_name, FILE_WRITE);
    if (!f)
      return;

    for (int i = 0; i < STAT_RANGE_COUNT; i++) {
      f.printf("%d;%.4f;%.4f\n",
               i, avgEfficiencyInc[i], avgEfficiencyDec[i]);
    }
    f.close();
  }

  void load() {
    if (_IsLoaded || !_FS_Initialized)
      return;

    _IsLoaded = true;

    char file_name[50];
    snprintf(file_name, sizeof(file_name), "/%s_stat.csv", SubTypeName());
    toLowerCase(file_name);

    File f = LittleFS.open(file_name, FILE_READ);
    if (!f) {
      debug.printf("Failed to open file %s for reading\n", file_name);
      return;
    }

    debug.printf("Loading statistics from %s\n", file_name);

    while (f.available()) {
      String line = f.readStringUntil('\n');
      int    sep1 = line.indexOf(';');
      int    sep2 = line.indexOf(';', sep1 + 1);

      if (sep1 != -1 && sep2 != -1) {
        int range = line.substring(0, sep1).toInt();
        if (range >= 0 && range < STAT_RANGE_COUNT) {
          avgEfficiencyInc[range] = line.substring(sep1 + 1, sep2).toFloat();
          avgEfficiencyDec[range] = line.substring(sep2 + 1).toFloat();

          debug.printf("Loaded range %d: inc=%.4f, dec=%.4f\n",
                       range, avgEfficiencyInc[range], avgEfficiencyDec[range]);
        }
      }
    }
    f.close();
  }

  void updateStatistics(int range, bool isIncrease, float efficiency) {
    float   *effBuf = isIncrease ? efficiencyInc[range] : efficiencyDec[range];
    uint8_t &idx    = isIncrease ? bufferIndexInc[range] : bufferIndexDec[range];
    float   &avgEff = isIncrease ? avgEfficiencyInc[range] : avgEfficiencyDec[range];

    _IsChanged = true;

    effBuf[idx] = efficiency;
    idx         = (idx + 1) % STAT_BUFFER_SIZE;

    float   sumEff = 0;
    uint8_t count  = 0;
    for (uint8_t i = 0; i < STAT_BUFFER_SIZE; i++) {
      if (effBuf[i] != 0) {
        sumEff += effBuf[i];
        count++;
      }
    }
    avgEff = count > 0 ? sumEff / count : 0;
  }

  bool isValidRange(int range) {
    return range >= 0 && range < STAT_RANGE_COUNT;
  }

public:
  DeviceController(_MainType MainType, _SubType SubType, ulong PredictMin = STAT_PREDICT_MIN, ulong PredictMax = STAT_PREDICT_MAX) {
    setType(MainType, SubType);
    setPredictMin(PredictMin);
    setPredictMax(PredictMax);
  }

  ~DeviceController() {
    save();
  }

  void put(float ValueFrom, float ValueTo, ulong WorkMillis, bool Increased) {
    if ((WorkMillis < 1) ||
        (Increased ? ValueTo < ValueFrom : ValueTo > ValueFrom))
      return;

    float efficiency = fabsf(ValueTo - ValueFrom) / (WorkMillis / 100.0f);

    int range = static_cast<int>(ValueFrom) / STAT_RANGE_STEP;
    if (!isValidRange(range))
      return;

    updateStatistics(range, Increased, efficiency);
  }

  ulong predict(float CurrentValue, float TargetValue) {
    if (CurrentValue == TargetValue)
      return 0;
    if (fabsf(TargetValue - CurrentValue) < 0.5f)
      return 0;

    int range = static_cast<int>(CurrentValue) / STAT_RANGE_STEP;
    if (!isValidRange(range))
      return 0;

    bool  isIncrease = TargetValue > CurrentValue;
    float efficiency = isIncrease ? avgEfficiencyInc[range] : avgEfficiencyDec[range];

    float neededChange = fabsf(TargetValue - CurrentValue);

    float _predict = (efficiency != 0) ? ((neededChange / efficiency) * 100) : 0;

    if (!std::isfinite(_predict) || (_predict < 1))
      return 0;

    float _clamped = constrain(_predict, static_cast<float>(_PredictMin), static_cast<float>(_PredictMax));

    return static_cast<ulong>(_clamped);
  }

  void dump() {
    debug.println("\nСтатистика:");
    for (int i = 0; i < STAT_RANGE_COUNT; i++) {
      debug.printf("Диапазон %d%%: ↑ %.2f, ↓ %.2f\n",
                   i * STAT_RANGE_STEP,
                   avgEfficiencyInc[i],
                   avgEfficiencyDec[i]);
    }
  }

  void Tick() {
    ulong currentTime = millis();
    if (((SaveMillis == 0) || (currentTime - SaveMillis) > SaveMillisInterval) && _FS_Initialized) {
      SaveMillis = currentTime;

      load();

      if (_IsChanged) {
        _IsChanged = false;

        save();
      }
    }
  }

  void setPredictMin(ulong Value) {
    _PredictMin = min(Value, _PredictMax);
  }

  void setPredictMax(ulong Value) {
    _PredictMax = max(Value, _PredictMin);
  }

  void setPredictLimits(ulong Min, ulong Max) {
    setPredictMin(Min);
    setPredictMax(Max);
  }

  ulong PredictMin() {
    return _PredictMin;
  }

  ulong PredictMax() {
    return _PredictMax;
  }
};

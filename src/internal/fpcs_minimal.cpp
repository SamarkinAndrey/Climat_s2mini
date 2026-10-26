#include "fpcs_minimal.h"

#include <math.h>

namespace {

// Вставляет индекс в отсортированный массив без дубликатов.
// Возвращает false, если такой индекс уже есть или массив заполнен.
bool addIndex(size_t *idx, size_t &cnt, size_t v) {
  for (size_t i = 0; i < cnt; i++) {
    if (idx[i] == v)
      return false;
  }
  if (cnt >= FPCS_MAX_OUT)
    return false;

  size_t pos = cnt;
  while (pos > 0 && idx[pos - 1] > v) {
    idx[pos] = idx[pos - 1];
    pos--;
  }
  idx[pos] = v;
  cnt++;
  return true;
}

bool hasIndex(const size_t *idx, size_t cnt, size_t v) {
  for (size_t i = 0; i < cnt; i++) {
    if (idx[i] == v)
      return true;
  }
  return false;
}

} // namespace

size_t fpcs_select(const Sample *in, size_t n, Sample *out, const FpcsConfig &cfg) {
  if (!in || !out || n == 0)
    return 0;

  size_t idx[FPCS_MAX_OUT];
  size_t cnt = 0;

  // 1. Границы окна включаются всегда.
  addIndex(idx, cnt, 0);
  addIndex(idx, cnt, n - 1);

  // 2. Влажность: находим экстремумы и число валидных замеров.
  //    h <= 0 или h > 100 — служебное «нет замера» / мусор, в экстремумы не идёт.
  float  hMin = 0, hMax = 0;
  size_t iHMin = 0, iHMax = 0;
  size_t hValid = 0;
  for (size_t i = 0; i < n; i++) {
    float h = in[i].h;
    if (!isfinite(h) || h <= 0.0f || h > 100.0f)
      continue;
    if (!hValid) {
      hMin = hMax = h;
      iHMin = iHMax = i;
    } else {
      if (h < hMin) { hMin = h; iHMin = i; }
      if (h > hMax) { hMax = h; iHMax = i; }
    }
    hValid++;
  }

  // 3. CO2: min/max/перегиб только если разброс выше порога шума.
  //    co2 == 0 — «нет валидного замера», в экстремумы не идёт.
  uint16_t cMin = 0, cMax = 0;
  size_t   iCMin = 0, iCMax = 0;
  size_t   cValid = 0;
  for (size_t i = 0; i < n; i++) {
    uint16_t c = in[i].co2;
    if (c == 0)
      continue;
    if (!cValid) {
      cMin = cMax = c;
      iCMin = iCMax = i;
    } else {
      if (c < cMin) { cMin = c; iCMin = i; }
      if (c > cMax) { cMax = c; iCMax = i; }
    }
    cValid++;
  }

  // 4. Приоритет при нехватке слотов: CO2-пик важнее H. Одиночный валидный
  //    замер H/CO2 добавляем всегда, даже если он не граница и не экстремум:
  //    иначе единственное реальное значение окна терялось бы (P3.6).
  if (hValid == 1)
    addIndex(idx, cnt, iHMax);
  if (cValid == 1)
    addIndex(idx, cnt, iCMax);

  const bool hSpread = (hValid >= 2 && (hMax - hMin) >= cfg.thH);
  const bool cSpread = (cValid >= 2 && (int)cMax - (int)cMin >= (int)cfg.thCo2);

  if (cSpread) {
    addIndex(idx, cnt, iCMax); // главный пик — раньше остальных
    if (hSpread)
      addIndex(idx, cnt, iHMax);
    addIndex(idx, cnt, iCMin);
    if (hSpread)
      addIndex(idx, cnt, iHMin);

    // «Перегиб» — точка с максимальной второй разностью (излом тренда).
    // Добавится только если после пиков остался слот: при жёстком лимите 4
    // границы + max/min CO2 обычно уже занимают всё.
    if (cnt < FPCS_MAX_OUT) {
      int    bestScore = -1;
      size_t bestIdx   = 0;
      for (size_t i = 1; i + 1 < n; i++) {
        if (in[i - 1].co2 == 0 || in[i].co2 == 0 || in[i + 1].co2 == 0)
          continue;
        if (hasIndex(idx, cnt, i))
          continue;
        int dPrev = (int)in[i].co2 - (int)in[i - 1].co2;
        int dNext = (int)in[i + 1].co2 - (int)in[i].co2;
        int score = abs(dNext - dPrev);
        if (score > bestScore) {
          bestScore = score;
          bestIdx   = i;
        }
      }
      // Излом меньше порога — это шум, а не фронт.
      if (bestScore >= (int)cfg.thCo2)
        addIndex(idx, cnt, bestIdx);
    }
  } else if (hSpread) {
    // Сначала максимум: при нехватке места он важнее (душ/проветривание).
    addIndex(idx, cnt, iHMax);
    addIndex(idx, cnt, iHMin);
  }

  // 5. Нижняя граница плотности: спокойное окно остаётся с двумя границами,
  //    что даёт 576 точек/сутки — меньше требуемых 600. Добавляем реальный
  //    серединный замер (не интерполяцию), получая 3 точки на окно (864/сутки).
  if (cnt < 3 && n >= 3)
    addIndex(idx, cnt, n / 2);

  // 6. T — одно среднее на окно. Проставляем его всем выбранным строкам:
  //    так температура не «дробится» на пики CO2/H, а остаётся одной точкой
  //    на окно (график T идёт ступеньками внутри пятиминутки).
  //    NaN — «нет замера»: такие строки не участвуют в среднем. Если валидной
  //    T не было ни одной, наружу уходит NaN, а не ложный 0 °C.
  double tSum = 0;
  size_t tValid = 0;
  for (size_t i = 0; i < n; i++) {
    if (!isfinite(in[i].t))
      continue;
    tSum += in[i].t;
    tValid++;
  }
  float tMean = tValid ? (float)(tSum / (double)tValid) : NAN;

  for (size_t k = 0; k < cnt; k++) {
    out[k]      = in[idx[k]];
    out[k].t    = tMean;
  }
  return cnt;
}

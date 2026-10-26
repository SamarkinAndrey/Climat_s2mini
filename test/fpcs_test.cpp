// Хост-тест модуля FPCS. В прошивку не входит: собирается обычным g++.
//   g++ -std=c++17 fpcs_test.cpp ../src/internal/fpcs_minimal.cpp -o fpcs_test
// (пути — от каталога test/, либо из корня проекта см. отчёт).

#include "../src/internal/fpcs_minimal.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const uint32_t BASE_TS = 1750000000UL;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    printf("    FAIL: %s\n", what);
    failures++;
  }
}

int noise(int i) { // детерминированный «шум» -10..+10
  return ((i * 37) % 21) - 10;
}

Sample make(size_t i, float t, float h, uint16_t co2) {
  Sample s;
  s.ts  = BASE_TS + (uint32_t)i * 5;
  s.t   = t;
  s.h   = h;
  s.co2 = co2;
  return s;
}

void printOut(const Sample *out, size_t n) {
  printf("    точек %u:", (unsigned)n);
  for (size_t i = 0; i < n; i++)
    printf(" [%u ts=+%us t=%.2f h=%.1f co2=%u]", (unsigned)i,
           (unsigned)(out[i].ts - BASE_TS), out[i].t, out[i].h, out[i].co2);
  printf("\n");
}

bool hasCo2(const Sample *out, size_t n, uint16_t v) {
  for (size_t i = 0; i < n; i++)
    if (out[i].co2 == v)
      return true;
  return false;
}

bool hasH(const Sample *out, size_t n, float v) {
  for (size_t i = 0; i < n; i++)
    if (std::fabs(out[i].h - v) < 1e-4f)
      return true;
  return false;
}

bool hasTs(const Sample *out, size_t n, uint32_t index) {
  const uint32_t ts = BASE_TS + index * 5;
  for (size_t i = 0; i < n; i++)
    if (out[i].ts == ts)
      return true;
  return false;
}

bool tsStrictlyIncreasing(const Sample *out, size_t n) {
  for (size_t i = 1; i < n; i++)
    if (out[i].ts <= out[i - 1].ts)
      return false;
  return true;
}

void scenarioCalm() {
  printf("[1] Спокойное окно: шум CO2 +-10 ppm, H +-1 %%\n");
  int    before = failures;
  Sample in[60];
  float  tSum = 0;
  for (size_t i = 0; i < 60; i++) {
    float t = 22.0f + 0.2f * (float)std::sin((double)i * 0.2);
    float h = 45.0f + (float)noise((int)i) / 10.0f; // 44..46, разброс <= 2
    in[i]   = make(i, t, h, (uint16_t)(800 + noise((int)i)));
    tSum += t;
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n == 3, "спокойное окно: 2 границы + серединный реальный замер = 3");
  check(n >= 1 && n <= FPCS_MAX_OUT, "число точек в пределах 1..4");
  check(tsStrictlyIncreasing(out, n), "ts строго возрастают");
  check(out[0].ts == BASE_TS, "первая точка — начало окна");
  check(out[n - 1].ts == BASE_TS + 59 * 5, "последняя точка — конец окна");
  check(out[1].ts == BASE_TS + 30 * 5, "середина окна — реальный замер i=30");
  check(out[1].co2 == in[30].co2, "середина: CO2 из замера, не интерполяция");
  check(std::fabs(out[1].h - in[30].h) < 1e-4f, "середина: H из замера");
  for (size_t i = 0; i < n; i++) {
    check(std::fabs(out[i].t - tSum / 60.0f) < 1e-4f, "T = среднее по окну");
    check(out[i].co2 >= 790 && out[i].co2 <= 810, "шум CO2 не размножен");
  }
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioCo2Peak() {
  printf("[2] Пик CO2: 800 -> 1500 -> 800 за 1 минуту (H тоже пикует в тот же момент)\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++) {
    float    t = 22.5f;
    float    h = 45.0f;
    uint16_t c = 800;
    if (i >= 20 && i <= 32) {
      float k = (i <= 26) ? (float)(i - 20) / 6.0f : (float)(32 - i) / 6.0f;
      c = (uint16_t)(800 + (int)(700.0f * k + 0.5f));
      h = 45.0f + 25.0f * k; // max H в той же точке, что max CO2
    }
    in[i] = make((size_t)i, t, h, c);
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n >= 3 && n <= FPCS_MAX_OUT, "пик должен добавить точки: 3..4");
  check(hasCo2(out, n, 1500), "max CO2 = 1500 ppm сохранён");
  check(hasCo2(out, n, 800), "min CO2 = 800 ppm сохранён");
  check(tsStrictlyIncreasing(out, n), "ts строго возрастают");
  bool hPeak = false;
  int  bothPeak = 0;
  for (size_t i = 0; i < n; i++) {
    if (std::fabs(out[i].h - 70.0f) < 1e-4f)
      hPeak = true;
    if (std::fabs(out[i].h - 70.0f) < 1e-4f && out[i].co2 == 1500)
      bothPeak++;
  }
  check(hPeak, "max H = 70 % сохранён");
  check(bothPeak == 1, "min H и max CO2 в один момент — одна строка, а не две");
  check(out[0].co2 == 800 && out[n - 1].co2 == 800, "границы сохранены");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioHumidityPeak() {
  printf("[3] Пик H: 45 -> 70 -> 45 %% (CO2 спокойный)\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++) {
    float k = 0.0f;
    if (i >= 20 && i <= 32)
      k = (i <= 26) ? (float)(i - 20) / 6.0f : (float)(32 - i) / 6.0f;
    in[i] = make((size_t)i, 21.0f, 45.0f + 25.0f * k, (uint16_t)(800 + noise(i)));
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n >= 3 && n <= FPCS_MAX_OUT, "пик H должен добавить точки: 3..4");
  float hMin = 1000, hMax = -1000;
  int   spikes = 0;
  for (size_t i = 0; i < n; i++) {
    if (out[i].h < hMin) hMin = out[i].h;
    if (out[i].h > hMax) hMax = out[i].h;
    if (out[i].co2 > 810) spikes++;
  }
  check(std::fabs(hMax - 70.0f) < 1e-4f, "max H = 70 % сохранён");
  check(std::fabs(hMin - 45.0f) < 1e-4f, "min H = 45 % сохранён");
  check(spikes == 0, "шум CO2 не породил ложных пиков");
  check(tsStrictlyIncreasing(out, n), "ts строго возрастают");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioSawtooth() {
  printf("[4] Пила: CO2 800 -> 1380 -> 800, H 40 -> 55 -> 40\n");
  int    before = failures;
  Sample in[60];
  float  tSum = 0;
  for (int i = 0; i < 60; i++) {
    float t = 20.0f + 0.05f * (float)i;
    float h = (i <= 29) ? 40.0f + 0.5f * (float)i : 40.0f + 0.5f * (float)(59 - i);
    int   c = (i <= 29) ? 800 + 20 * i : 800 + 20 * (59 - i);
    in[i]   = make((size_t)i, t, h, (uint16_t)c);
    tSum += t;
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n >= 2 && n <= FPCS_MAX_OUT, "пила укладывается в 2..4 точки");
  check(hasCo2(out, n, 1380), "max CO2 = 1380 ppm сохранён");
  check(hasCo2(out, n, 800), "min CO2 = 800 ppm сохранён");
  check(tsStrictlyIncreasing(out, n), "ts строго возрастают");
  float m = tSum / 60.0f;
  bool  tOk = true;
  for (size_t i = 0; i < n; i++)
    if (std::fabs(out[i].t - m) > 1e-4f)
      tOk = false;
  check(tOk, "T = среднее по окну");
  float hMax = 0;
  for (size_t i = 0; i < n; i++)
    if (out[i].h > hMax) hMax = out[i].h;
  check(std::fabs(hMax - 54.5f) < 1e-4f, "max H = 54.5 % сохранён");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioMissingTh() {
  printf("[5] Пропуски T/H: нет замера в каждой третьей строке\n");
  int    before = failures;
  Sample in[60];
  double tSum = 0;
  int    tValid = 0;
  for (int i = 0; i < 60; i++) {
    bool  miss = (i % 3 == 0);
    float t    = miss ? NAN : 20.0f + 0.01f * (float)i;
    float h    = miss ? NAN : 45.0f;
    in[i]      = make((size_t)i, t, h, (uint16_t)(800 + noise(i)));
    if (!miss) {
      tSum += t;
      tValid++;
    }
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n == 3, "пропуски T/H дают 3 точки: границы + середина");
  float mean = (float)(tSum / (double)tValid);
  bool  tOk  = (n > 0);
  bool  hOk  = true;
  bool  hSeen = false;
  for (size_t i = 0; i < n; i++) {
    if (!std::isfinite(out[i].t) || std::fabs(out[i].t - mean) > 1e-4f)
      tOk = false;
    if (std::isnan(out[i].h))
      continue; // граница/середина окна могла быть без замера — это нормально
    hSeen = true;
    if (std::fabs(out[i].h - 45.0f) > 1e-4f)
      hOk = false;
  }
  if (!hSeen)
    hOk = false;
  check(tOk, "T = среднее только по валидным замерам");
  check(hOk, "NaN H не превращается в экстремум/0");
  check(std::isnan(out[1].h), "середина (i=30) без DHT остаётся NaN, а не 0");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioNoValidT() {
  printf("[6] Нет валидной T/H вовсе: наружу должен уйти NaN\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++)
    in[i] = make((size_t)i, NAN, NAN, (uint16_t)(800 + noise(i)));
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n == 3, "без T/H: границы + середина, всего 3");
  bool noT = (n > 0), noH = (n > 0);
  for (size_t i = 0; i < n; i++) {
    if (!std::isnan(out[i].t))
      noT = false;
    if (!std::isnan(out[i].h))
      noH = false;
  }
  check(noT, "T отсутствует -> NaN, а не 0 °C");
  check(noH, "H отсутствует -> NaN");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioEdges() {
  printf("[7] Края: n=0, n=1, n=2\n");
  int    before = failures;
  Sample in[2];
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  in[0] = make(0, 20.0f, 45.0f, 800);
  in[1] = make(1, 21.0f, 46.0f, 900);

  check(fpcs_select(nullptr, 0, out, cfg) == 0, "nullptr/n=0 -> 0 точек");
  check(fpcs_select(in, 0, out, cfg) == 0, "n=0 -> 0 точек");

  size_t n = fpcs_select(in, 1, out, cfg);
  check(n == 1, "n=1 -> одна точка");
  check(out[0].ts == BASE_TS && out[0].co2 == 800, "n=1: сохранён единственный замер");
  check(std::fabs(out[0].t - 20.0f) < 1e-4f, "n=1: T = единственное значение");

  n = fpcs_select(in, 2, out, cfg);
  check(n == 2, "n=2 -> обе границы");
  check(out[0].ts == BASE_TS && out[1].ts == BASE_TS + 5, "n=2: границы на месте");
  check(std::fabs(out[0].t - 20.5f) < 1e-4f, "n=2: T = среднее двух замеров");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioAllZero() {
  printf("[8] Окно целиком из co2=0 / h=0\n");
  int    before = failures;
  Sample in[60];
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;

  for (int i = 0; i < 60; i++)
    in[i] = make((size_t)i, 22.0f, 45.0f, 0);
  size_t n = fpcs_select(in, 60, out, cfg);
  check(n == 3, "нет CO2: границы + середина, без ложных экстремумов");
  for (size_t i = 0; i < n; i++) {
    check(out[i].co2 == 0, "co2=0 не превращается в экстремум");
    check(std::fabs(out[i].h - 45.0f) < 1e-4f, "валидная H сохраняется");
  }

  for (int i = 0; i < 60; i++)
    in[i] = make((size_t)i, 22.0f, 0.0f, 800);
  n = fpcs_select(in, 60, out, cfg);
  check(n == 3, "нет H: границы + середина, без ложных экстремумов");
  for (size_t i = 0; i < n; i++) {
    check(out[i].h == 0.0f, "h=0 остаётся отсутствием замера");
    check(out[i].co2 == 800, "валидный CO2 сохраняется");
  }
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioSingleValidH() {
  printf("[9] Единственный валидный H не на границе\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++)
    in[i] = make((size_t)i, 22.0f, (i == 17) ? 55.0f : 0.0f, 800);
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n >= 3 && n <= FPCS_MAX_OUT, "единственный H добавляет точку");
  check(hasH(out, n, 55.0f), "единственный валидный H сохранён");
  check(hasTs(out, n, 17), "сохранён именно замер i=17");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioSingleValidCo2() {
  printf("[10] Единственный валидный CO2 не на границе\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++)
    in[i] = make((size_t)i, 22.0f, 45.0f, (i == 23) ? 1200 : 0);
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n >= 3 && n <= FPCS_MAX_OUT, "единственный CO2 добавляет точку");
  check(hasCo2(out, n, 1200), "единственный валидный CO2 сохранён");
  check(hasTs(out, n, 23), "сохранён именно замер i=23");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioHardLimit() {
  printf("[11] Жёсткий лимит: много разных экстремумов H и CO2\n");
  int    before = failures;
  Sample in[60];
  for (int i = 0; i < 60; i++) {
    float    h = 45.0f;
    uint16_t c = 800;
    if (i == 10) h = 30.0f; // min H
    if (i == 40) h = 70.0f; // max H
    if (i == 15) c = 1500;  // max CO2
    if (i == 30) c = 400;   // min CO2
    in[i] = make((size_t)i, 22.0f, h, c);
  }
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  size_t n = fpcs_select(in, 60, out, cfg);
  printOut(out, n);

  check(n == FPCS_MAX_OUT, "выход не превышает FPCS_MAX_OUT");
  check(hasCo2(out, n, 1500), "при лимите сохраняется max CO2");
  check(hasH(out, n, 70.0f), "при лимите сохраняется max H");
  check(out[0].co2 == 800 && out[n - 1].co2 == 800, "границы сохранены");
  check(tsStrictlyIncreasing(out, n), "ts строго возрастают, дубликатов нет");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

void scenarioDailyBudget() {
  printf("[12] Суточный бюджет: 288 окон, спокойные и с пиками\n");
  int    before = failures;
  Sample in[60];
  Sample out[FPCS_MAX_OUT];
  FpcsConfig cfg;
  uint32_t totalCalm = 0;
  uint32_t totalPeak = 0;

  for (int w = 0; w < 288; w++) {
    // спокойное окно: шум ниже порогов -> 3 точки
    for (int i = 0; i < 60; i++)
      in[i] = make((size_t)i, 22.0f, 45.0f, (uint16_t)(800 + noise(i)));
    totalCalm += (uint32_t)fpcs_select(in, 60, out, cfg);

    // окно с пиком CO2/H -> 4 точки
    for (int i = 0; i < 60; i++) {
      float    h = 45.0f;
      uint16_t c = 800;
      if (i >= 20 && i <= 32) {
        float k = (i <= 26) ? (float)(i - 20) / 6.0f : (float)(32 - i) / 6.0f;
        c = (uint16_t)(800 + (int)(700.0f * k + 0.5f));
        h = 45.0f + 25.0f * k;
      }
      in[i] = make((size_t)i, 22.0f, h, c);
    }
    totalPeak += (uint32_t)fpcs_select(in, 60, out, cfg);
  }

  printf("    спокойные сутки: %u точек, сутки с пиками: %u точек\n",
         (unsigned)totalCalm, (unsigned)totalPeak);
  check(totalCalm == 288 * 3, "спокойные сутки = 864 точки");
  check(totalPeak == 288 * 4, "сутки с пиками = 1152 точки (лимит 4/окно)");
  check(totalCalm >= 600 && totalPeak <= 1200, "объём в сутки 600..1200");
  printf("    %s\n", failures == before ? "PASS" : "FAIL");
}

} // namespace

int main() {
  scenarioCalm();
  scenarioCo2Peak();
  scenarioHumidityPeak();
  scenarioSawtooth();
  scenarioMissingTh();
  scenarioNoValidT();
  scenarioEdges();
  scenarioAllZero();
  scenarioSingleValidH();
  scenarioSingleValidCo2();
  scenarioHardLimit();
  scenarioDailyBudget();

  if (failures) {
    printf("\nИТОГ: %d проверок провалено\n", failures);
    return 1;
  }
  printf("\nИТОГ: все 12 сценариев пройдены\n");
  return 0;
}

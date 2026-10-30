#pragma once

#include <stddef.h>
#include <stdint.h>

struct Sample {
  uint32_t ts;
  float    t;
  float    h;
  uint16_t co2;
};

struct FpcsConfig {
  float    thH   = 5.0f;
  uint16_t thCo2 = 50;
};

static const size_t FPCS_MAX_OUT = 4;

size_t fpcs_select(const Sample *in, size_t n, Sample *out, const FpcsConfig &cfg);

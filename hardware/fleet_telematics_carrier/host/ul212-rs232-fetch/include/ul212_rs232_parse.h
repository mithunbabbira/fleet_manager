#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ul212Reading {
  bool valid;
  float height_mm;
  float smooth_mm;
  float temp_c;
  uint8_t tilt_deg;
  uint8_t signal;
  uint32_t stamp_ms;
} Ul212Reading;

bool ul212ParseXd(const char *frame, Ul212Reading *out);
bool ul212ParseCfv(const char *frame, Ul212Reading *out);

#ifdef __cplusplus
}
#endif

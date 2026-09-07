#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Same shape as RS-232/BLE Ul212Reading — local copy so this project stays standalone. */
typedef struct DummyFuelReading {
  bool valid;
  float height_mm;
  float smooth_mm;
  float temp_c;
  uint8_t tilt_deg;
  uint8_t signal;
  uint32_t stamp_ms;
} DummyFuelReading;

#ifdef __cplusplus
}
#endif

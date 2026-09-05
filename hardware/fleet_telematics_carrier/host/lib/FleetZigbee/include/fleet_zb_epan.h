#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parse 16 hex digits (optional 0x) MSB-first into little-endian 8 bytes. */
bool fleet_zb_epan_parse(const char *hex16, uint8_t out_le[8]);

/** Format LE 8 bytes to 16 uppercase hex chars + NUL (MSB-first). */
void fleet_zb_epan_format(const uint8_t in_le[8], char out[17]);

#ifdef __cplusplus
}
#endif

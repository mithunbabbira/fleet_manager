#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    const char *name;
    const char *unit;
    double value;
    char raw_hex[48];
} obd_decoded_t;

/** @brief Decode Mode 01 PID response into name/unit/value. */
bool obd_codec_decode_mode01(const char *response, uint8_t pid, obd_decoded_t *out);
/** @brief Parse DTC codes from Mode 03/07/0A response; returns count. */
int obd_codec_parse_dtcs(const char *response, char out[][6], int max_out);
/** @brief Extract 17-char VIN from Mode 09 02 response. */
bool obd_codec_parse_vin(const char *response, char *vin, size_t vin_len);
/** @brief Decode by profile key (rpm/speed/…/voltage) into obd_decoded_t. */
bool obd_codec_decode_named(const char *decode_key, const char *response, obd_decoded_t *out);

#ifdef __cplusplus
}
#endif

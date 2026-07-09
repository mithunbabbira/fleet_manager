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

bool obd_codec_decode_mode01(const char *response, uint8_t pid, obd_decoded_t *out);
int obd_codec_parse_dtcs(const char *response, char out[][6], int max_out);
bool obd_codec_parse_vin(const char *response, char *vin, size_t vin_len);
bool obd_codec_decode_named(const char *decode_key, const char *response, obd_decoded_t *out);

#ifdef __cplusplus
}
#endif

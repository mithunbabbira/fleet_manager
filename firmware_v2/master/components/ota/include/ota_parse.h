#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OTA_CHECK_UPDATE = 0,
    OTA_CHECK_NO_UPDATE,
    OTA_CHECK_FAIL,
} ota_check_kind_t;

typedef struct {
    ota_check_kind_t kind;
    char error[96];
    char latest_version[40];
    char presigned_url[1024];
    char sha256[65];
    size_t size;
    bool update_available;
} ota_check_result_t;

/**
 * @brief Strip first "-suffix" from version for Trafyn currentVersion compare.
 * @note Only first dash; "1.0.4-rc.1" → "1.0.4".
 */
void ota_strip_version(const char *in, char *out, size_t out_len);

/**
 * @brief Parse Trafyn check JSON → UPDATE / NO_UPDATE / FAIL.
 * @param stripped_current Must be non-NULL (else UB on strcmp).
 * @note UPDATE needs updateAvailable, version≠current, URL, 64-hex sha256, size>0.
 */
int ota_parse_check_json(const char *json, const char *stripped_current,
                            ota_check_result_t *out);

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
 * @brief Parse Trafyn check JSON → UPDATE / NO_UPDATE / FAIL.
 * @param current_version Full, unmodified running app version; must be
 *        non-NULL (else UB on strcmp). Compared as-is against the response's
 *        raw latestVersion — any difference at all counts as a mismatch, so
 *        a rebuild with a different build-number suffix is always an update.
 * @note UPDATE needs updateAvailable, version≠current, URL, 64-hex sha256, size>0.
 */
int ota_parse_check_json(const char *json, const char *current_version,
                            ota_check_result_t *out);

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FW_OTA_CHECK_UPDATE = 0,
    FW_OTA_CHECK_NO_UPDATE,
    FW_OTA_CHECK_FAIL,
} fw_ota_check_kind_t;

typedef struct {
    fw_ota_check_kind_t kind;
    char error[96];
    char latest_version[40];
    char presigned_url[1024];
    char sha256[65];
    size_t size;
    bool update_available;
} fw_ota_check_result_t;

void fw_ota_strip_version(const char *in, char *out, size_t out_len);
int fw_ota_parse_check_json(const char *json, const char *stripped_current,
                            fw_ota_check_result_t *out);

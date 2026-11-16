#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format the factory/base Wi-Fi MAC as 12 uppercase hex digits (no colons).
 * @example "58E6C5DBCD84"
 * @note out_len must be at least 13.
 */
esp_err_t board_get_mac_id(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

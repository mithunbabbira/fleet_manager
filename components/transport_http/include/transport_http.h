#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up SoftAP (Fleet-C6) and HTTP server on port 80.
 *
 * SoftAP serves stats/config UI only — firmware bins are not uploaded over SoftAP.
 */
esp_err_t transport_http_start(void);

#ifdef __cplusplus
}
#endif

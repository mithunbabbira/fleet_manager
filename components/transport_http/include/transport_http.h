#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the SoftAP (Kconfig ELM_SOFTAP_SSID/ELM_SOFTAP_PASS), start the
 * embedded HTTP server on port 80, and register the REST API + HTML UI.
 * Safe to call once, after nvs_flash_init()/profile_store_init() etc.
 */
esp_err_t transport_http_start(void);

#ifdef __cplusplus
}
#endif

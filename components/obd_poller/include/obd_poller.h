#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t obd_poller_start(void);
esp_err_t obd_poller_stop(void);
esp_err_t obd_poller_reload_active_profile(void);
esp_err_t obd_poller_submit_raw(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

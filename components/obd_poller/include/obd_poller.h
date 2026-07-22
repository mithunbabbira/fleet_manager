#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t obd_poller_start(void);
esp_err_t obd_poller_stop(void);
esp_err_t obd_poller_reload_active_profile(void);
/** When false, only raw submit_raw requests run (no profile PID polling). */
void obd_poller_set_enabled(bool enabled);
bool obd_poller_is_enabled(void);
esp_err_t obd_poller_submit_raw(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

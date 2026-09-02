#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start poller task (profile PID poll + raw cmd queue via can_obd). */
esp_err_t obd_poller_start(void);
/** @brief Stop poller task and drain pending raw requests. */
esp_err_t obd_poller_stop(void);
/** @brief Reload active profile from profile_store into poller state. */
esp_err_t obd_poller_reload_active_profile(void);
/** When false, only raw submit_raw requests run (no profile PID polling). */
void obd_poller_set_enabled(bool enabled);
/** @brief True when profile PID polling is enabled. */
bool obd_poller_is_enabled(void);
/** @brief Queue a raw OBD/AT cmd; blocks until poller completes transaction. */
esp_err_t obd_poller_submit_raw(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

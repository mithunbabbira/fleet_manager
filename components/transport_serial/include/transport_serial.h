#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start USB console task (status/lte/uplink/ota/cmd and related commands). */
esp_err_t transport_serial_start(void);

#ifdef __cplusplus
}
#endif

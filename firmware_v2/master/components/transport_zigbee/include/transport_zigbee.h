#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init registry and (when enabled) the Zigbee coordinator radio. */
esp_err_t transport_zigbee_init(void);

/** Start receiving TLV frames from hosts. */
esp_err_t transport_zigbee_start(void);

/**
 * Feed a raw TLV frame as if it arrived over Zigbee.
 * Used by the radio RX path and by `fleet ingest` on the USB console.
 */
esp_err_t transport_zigbee_ingest(const uint8_t *frame, size_t len, uint16_t short_addr);

bool transport_zigbee_is_running(void);

#ifdef __cplusplus
}
#endif

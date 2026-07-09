#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the interactive serial console task.
 *
 * Reads line-based commands from stdin (UART0 / USB-Serial-JTOG console,
 * whichever CONFIG_ESP_CONSOLE_* selects) and drives the BLE/OBD stack
 * through the public APIs of ble_elm, elm327_client, obd_poller and
 * profile_store.
 *
 * This call assumes `profile_store_init()`, `telemetry_bus_init()`,
 * `ble_elm_init()`, `elm327_client_init()` and `obd_poller_start()` have
 * already been performed by the caller (see Task 11 boot sequence). It only
 * starts the console task itself; it does not initialize the rest of the
 * stack. Commands that depend on an uninitialized subsystem simply fail
 * gracefully (e.g. `scan` before `ble_elm_init()` returns an error that is
 * printed to the console instead of crashing).
 *
 * Safe to call once; calling it again while the task is already running is
 * a no-op.
 */
esp_err_t transport_serial_start(void);

#ifdef __cplusplus
}
#endif

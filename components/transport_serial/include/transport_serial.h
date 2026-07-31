#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the interactive CAN OBD serial console task.
 *
 * Reads line-based commands from the USB-Serial/JTAG console and drives the
 * direct CAN OBD stack through the public APIs of can_obd, obd_poller and
 * profile_store. This local console complements the SoftAP HTTP console.
 *
 * This call assumes `profile_store_init()`, `telemetry_bus_init()`,
 * `can_obd_init()`, `can_obd_start()` and `obd_poller_start()` have already
 * been performed by the caller. It only starts the console task itself; it
 * does not initialize the rest of the stack. Commands that depend on an
 * unavailable subsystem report the returned error.
 *
 * Safe to call once; calling it again while the task is already running is
 * a no-op.
 */
esp_err_t transport_serial_start(void);

#ifdef __cplusplus
}
#endif

#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register all REST API handlers (under /api) plus the "/" HTML page on
 * `server`, and start the telemetry-cache subscriber task backing
 * /api/telemetry. Call once, after the httpd server has been started.
 */
esp_err_t http_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register REST handlers under /api/ and the HTML UI on an already-started httpd. */
esp_err_t http_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

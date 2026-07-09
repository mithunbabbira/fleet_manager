#pragma once
#include "elm_transport.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t elm327_client_init(void);
esp_err_t elm327_client_set_transport(elm_transport_t *transport);
esp_err_t elm327_client_run_init_sequence(const char init_at[][16], int count);
esp_err_t elm327_client_transact(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);
bool elm327_client_is_ready(void);

#ifdef __cplusplus
}
#endif

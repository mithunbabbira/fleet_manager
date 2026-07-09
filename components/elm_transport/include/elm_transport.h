#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct elm_transport {
    void *ctx;
    esp_err_t (*write)(struct elm_transport *t, const uint8_t *data, size_t len);
    esp_err_t (*read_line)(struct elm_transport *t, char *buf, size_t buflen, uint32_t timeout_ms);
    bool (*is_ready)(struct elm_transport *t);
} elm_transport_t;

#ifdef __cplusplus
}
#endif

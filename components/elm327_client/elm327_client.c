#include "elm327_client.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <string.h>

static const char *TAG = "elm327_client";

static SemaphoreHandle_t s_mutex;
static elm_transport_t *s_transport;
static bool s_initialized;

static esp_err_t classify_response(const char *resp)
{
    if (strstr(resp, "NO DATA") != NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strstr(resp, "UNABLE TO CONNECT") != NULL || strstr(resp, "ERROR") != NULL) {
        return ESP_FAIL;
    }

    const char *p = resp;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }
    if (p[0] == '?' && p[1] == '\0') {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t elm327_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_transport = NULL;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t elm327_client_set_transport(elm_transport_t *transport)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_transport = transport;
    return ESP_OK;
}

bool elm327_client_is_ready(void)
{
    return s_transport != NULL && s_transport->is_ready != NULL &&
           s_transport->is_ready(s_transport);
}

esp_err_t elm327_client_transact(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cmd == NULL || resp == NULL || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ELM_CMD_TIMEOUT_MS;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;

    if (!elm327_client_is_ready()) {
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }

    char cmd_buf[128];
    size_t cmd_len = strlen(cmd);
    if (cmd_len >= sizeof(cmd_buf) - 1) {
        result = ESP_ERR_INVALID_ARG;
        goto out;
    }

    memcpy(cmd_buf, cmd, cmd_len);
    if (cmd_len == 0 || cmd_buf[cmd_len - 1] != '\r') {
        cmd_buf[cmd_len++] = '\r';
    }
    cmd_buf[cmd_len] = '\0';

    result = s_transport->write(s_transport, (const uint8_t *)cmd_buf, cmd_len);
    if (result != ESP_OK) {
        goto out;
    }

    resp[0] = '\0';
    result = s_transport->read_line(s_transport, resp, resp_len, timeout_ms);
    if (result == ESP_ERR_TIMEOUT || resp[0] == '\0') {
        resp[0] = '\0';
        result = ESP_ERR_TIMEOUT;
        goto out;
    }
    if (result != ESP_OK) {
        goto out;
    }

    result = classify_response(resp);

out:
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t elm327_client_run_init_sequence(const char init_at[][16], int count)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (init_at == NULL || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char resp[128];

    for (int i = 0; i < count; ++i) {
        esp_err_t err = elm327_client_transact(init_at[i], resp, sizeof(resp),
                                               CONFIG_ELM_CMD_TIMEOUT_MS);
        ESP_LOGI(TAG, "init step %d: %s -> %s (%s)", i, init_at[i], resp, esp_err_to_name(err));

        if (err == ESP_FAIL || err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "init sequence failed at step %d", i);
            return err;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init sequence aborted at step %d: %s", i, esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

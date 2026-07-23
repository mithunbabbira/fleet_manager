#include "net_lte.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "net_lte";

#if CONFIG_NET_LTE_ENABLE

/* Modem can take several seconds to boot; retry AT for up to this long. */
#define NET_LTE_AT_ATTEMPTS   40
#define NET_LTE_AT_INTERVAL_MS 500

static net_lte_status_t s_status;
static bool s_uart_ready;
static SemaphoreHandle_t s_uart_mutex;
static TaskHandle_t s_bringup_task;

static esp_err_t at_transact_locked(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    if (resp && resp_len) {
        resp[0] = '\0';
    }

    /* Drain stale bytes. */
    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_NET_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }

    char line[96];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    if (n <= 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(CONFIG_NET_LTE_UART_PORT, line, n);
    if (written != n) {
        return ESP_FAIL;
    }

    size_t used = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_NET_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        if (resp && used + 1 < resp_len) {
            resp[used++] = (char)ch;
            resp[used] = '\0';
        }
        if (used >= 2) {
            if (strstr(resp, "OK") != NULL || strstr(resp, "ERROR") != NULL ||
                strstr(resp, "READY") != NULL) {
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t at_transact(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    if (s_uart_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = at_transact_locked(cmd, resp, resp_len, timeout_ms);
    xSemaphoreGive(s_uart_mutex);
    return err;
}

static esp_err_t uart_init(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

    uart_config_t cfg = {
        .baud_rate = CONFIG_NET_LTE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(CONFIG_NET_LTE_UART_PORT, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(CONFIG_NET_LTE_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_NET_LTE_UART_PORT,
                                 CONFIG_NET_LTE_UART_TX_GPIO,
                                 CONFIG_NET_LTE_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    s_uart_ready = true;
    ESP_LOGI(TAG, "UART%d ready TX=GPIO%d RX=GPIO%d baud=%d",
             CONFIG_NET_LTE_UART_PORT,
             CONFIG_NET_LTE_UART_TX_GPIO,
             CONFIG_NET_LTE_UART_RX_GPIO,
             CONFIG_NET_LTE_UART_BAUD);
    return ESP_OK;
}

static void bringup_task(void *arg)
{
    (void)arg;
    char resp[256];

    strncpy(s_status.last_error, "probing modem AT (waiting for boot)...",
            sizeof(s_status.last_error) - 1);

    bool at_ok = false;
    for (int attempt = 1; attempt <= NET_LTE_AT_ATTEMPTS; ++attempt) {
        if (at_transact("AT", resp, sizeof(resp), 1000) == ESP_OK && strstr(resp, "OK")) {
            at_ok = true;
            ESP_LOGI(TAG, "AT OK after %d attempt(s) (wiring looks good)", attempt);
            break;
        }
        if (attempt == 1 || attempt % 4 == 0) {
            ESP_LOGW(TAG, "no AT yet (attempt %d/%d); modem may still be booting",
                     attempt, NET_LTE_AT_ATTEMPTS);
        }
        vTaskDelay(pdMS_TO_TICKS(NET_LTE_AT_INTERVAL_MS));
    }

    if (!at_ok) {
        s_status.uart_ok = false;
        strncpy(s_status.last_error, "no AT: check TX/RX swap, GND, modem power",
                sizeof(s_status.last_error) - 1);
        s_status.last_error[sizeof(s_status.last_error) - 1] = '\0';
        ESP_LOGE(TAG, "no AT OK on UART. Check TX/RX cross, common GND, "
                      "and that the EC200U is powered and booted (PWRKEY).");
        s_bringup_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_status.uart_ok = true;

    if (at_transact("ATI", resp, sizeof(resp), 1500) == ESP_OK) {
        const char *p = strstr(resp, "EC200");
        if (p == NULL) {
            p = strstr(resp, "Quectel");
        }
        if (p != NULL) {
            strncpy(s_status.ati, p, sizeof(s_status.ati) - 1);
            for (char *c = s_status.ati; *c; ++c) {
                if (*c == '\r' || *c == '\n') {
                    *c = ' ';
                }
            }
        }
        ESP_LOGI(TAG, "ATI: %s", resp);
    }

    strncpy(s_status.last_error, "UART AT OK; PPP not implemented yet",
            sizeof(s_status.last_error) - 1);
    ESP_LOGW(TAG, "UART path verified. PPP/Internet still TODO.");

    net_lte_refresh();
    ESP_LOGI(TAG, "modem: sim_ready=%d reg=%d attached=%d csq=%d op='%s'",
             s_status.sim_ready, s_status.registered, s_status.attached,
             s_status.csq, s_status.operator_name);

    s_bringup_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t net_lte_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = true;
    strncpy(s_status.apn, CONFIG_NET_LTE_APN, sizeof(s_status.apn) - 1);

    if (s_uart_mutex == NULL) {
        s_uart_mutex = xSemaphoreCreateMutex();
        if (s_uart_mutex == NULL) {
            strncpy(s_status.last_error, "mutex alloc failed", sizeof(s_status.last_error) - 1);
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = uart_init();
    if (err != ESP_OK) {
        strncpy(s_status.last_error, "uart_init failed", sizeof(s_status.last_error) - 1);
        ESP_LOGE(TAG, "%s: %s", s_status.last_error, esp_err_to_name(err));
        return err;
    }

    if (s_bringup_task != NULL) {
        ESP_LOGW(TAG, "bring-up already running");
        return ESP_OK;
    }

    /* Non-blocking: bring-up runs in the background so SoftAP/BLE boot is not
     * delayed while the modem powers up. */
    if (xTaskCreate(bringup_task, "lte_bringup", 4096, NULL, 4, &s_bringup_task) != pdPASS) {
        strncpy(s_status.last_error, "bringup task spawn failed",
                sizeof(s_status.last_error) - 1);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "modem bring-up started in background (retries up to %d s)",
             (NET_LTE_AT_ATTEMPTS * NET_LTE_AT_INTERVAL_MS) / 1000);
    return ESP_OK;
}

static void parse_csq(const char *resp)
{
    const char *p = strstr(resp, "+CSQ:");
    if (!p) {
        return;
    }
    int rssi = 99, ber = 0;
    if (sscanf(p + 5, " %d,%d", &rssi, &ber) >= 1) {
        s_status.csq = rssi;
        if (rssi >= 0 && rssi <= 31) {
            s_status.rssi_dbm = -113 + 2 * rssi;
        } else {
            s_status.rssi_dbm = 0;
        }
    }
}

static void parse_cops(const char *resp)
{
    const char *q = strchr(resp, '"');
    if (!q) {
        return;
    }
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < sizeof(s_status.operator_name)) {
        s_status.operator_name[i++] = *q++;
    }
    s_status.operator_name[i] = '\0';
}

esp_err_t net_lte_refresh(void)
{
    if (!s_uart_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char resp[160];
    if (at_transact("AT+CPIN?", resp, sizeof(resp), 2000) == ESP_OK) {
        s_status.sim_ready = strstr(resp, "READY") != NULL;
    }
    if (at_transact("AT+CSQ", resp, sizeof(resp), 2000) == ESP_OK) {
        parse_csq(resp);
    }
    if (at_transact("AT+COPS?", resp, sizeof(resp), 3000) == ESP_OK) {
        parse_cops(resp);
    }
    if (at_transact("AT+CEREG?", resp, sizeof(resp), 2000) == ESP_OK ||
        at_transact("AT+CREG?", resp, sizeof(resp), 2000) == ESP_OK) {
        /* "+CxREG: <n>,<stat>" — stat 1 = home, 5 = roaming */
        const char *p = strchr(resp, ',');
        if (p) {
            int stat = atoi(p + 1);
            s_status.registered = (stat == 1 || stat == 5);
        }
    }
    if (at_transact("AT+CGATT?", resp, sizeof(resp), 2000) == ESP_OK) {
        const char *p = strstr(resp, "+CGATT:");
        if (p) {
            s_status.attached = (atoi(p + 7) == 1) || strstr(p, "1") != NULL;
        }
    }
    return ESP_OK;
}

static esp_err_t at_collect_locked(char *resp, size_t resp_len, int window_ms)
{
    size_t used = 0;
    if (resp && resp_len) {
        resp[0] = '\0';
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(window_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_NET_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        if (resp && used + 1 < resp_len) {
            resp[used++] = (char)ch;
            resp[used] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t net_lte_selftest(char *report, size_t len)
{
    if (report && len) {
        report[0] = '\0';
    }
    if (!s_uart_ready || s_uart_mutex == NULL) {
        if (report) {
            snprintf(report, len, "UART not ready");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(120000)) != pdTRUE) {
        if (report) {
            snprintf(report, len, "modem busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    char resp[256];
    size_t off = 0;
    esp_err_t rc = ESP_OK;

#define RPT(...)                                                            \
    do {                                                                    \
        if (report && off < len) {                                          \
            int _w = snprintf(report + off, len - off, __VA_ARGS__);        \
            if (_w > 0) {                                                   \
                off += (size_t)_w;                                          \
            }                                                               \
            if (off > len) {                                                \
                off = len;                                                  \
            }                                                               \
        }                                                                   \
    } while (0)

    ESP_LOGI(TAG, "selftest: start");
    if (at_transact_locked("AT", resp, sizeof(resp), 1000) == ESP_OK && strstr(resp, "OK")) {
        RPT("AT: OK\n");
        ESP_LOGI(TAG, "selftest: AT OK");
    } else {
        RPT("AT: FAIL (UART)\n");
        ESP_LOGE(TAG, "selftest: AT FAIL");
        rc = ESP_FAIL;
        goto done;
    }

    at_transact_locked("AT+CPIN?", resp, sizeof(resp), 2000);
    s_status.sim_ready = strstr(resp, "READY") != NULL;
    RPT("SIM: %s\n", s_status.sim_ready ? "READY" : "NOT ready");
    ESP_LOGI(TAG, "selftest: SIM %s", s_status.sim_ready ? "READY" : "not ready");

    ESP_LOGI(TAG, "selftest: waiting for network registration (up to 20s)...");
    bool reg = false;
    for (int i = 0; i < 20; ++i) {
        if (at_transact_locked("AT+CEREG?", resp, sizeof(resp), 2000) == ESP_OK) {
            const char *p = strchr(resp, ',');
            int stat = p ? atoi(p + 1) : 0;
            if (stat == 1 || stat == 5) {
                reg = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_status.registered = reg;
    RPT("Register: %s\n", reg ? "registered" : "NOT registered");
    ESP_LOGI(TAG, "selftest: %s", reg ? "registered" : "NOT registered");

    if (at_transact_locked("AT+CSQ", resp, sizeof(resp), 2000) == ESP_OK) {
        parse_csq(resp);
    }
    RPT("Signal: csq=%d (%d dBm)\n", s_status.csq, s_status.rssi_dbm);

    if (at_transact_locked("AT+COPS?", resp, sizeof(resp), 3000) == ESP_OK) {
        parse_cops(resp);
    }
    RPT("Operator: %s\n", s_status.operator_name[0] ? s_status.operator_name : "-");
    ESP_LOGI(TAG, "selftest: csq=%d op='%s'", s_status.csq, s_status.operator_name);

    if (!reg) {
        RPT("=> not registered; stop (check power/antenna/SIM)\n");
        ESP_LOGW(TAG, "selftest: not registered; stopping");
        rc = ESP_ERR_INVALID_STATE;
        goto done;
    }

    at_transact_locked("AT+CGATT?", resp, sizeof(resp), 2000);
    s_status.attached = strstr(resp, "+CGATT: 1") != NULL;
    if (!s_status.attached) {
        at_transact_locked("AT+CGATT=1", resp, sizeof(resp), 10000);
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",0", CONFIG_NET_LTE_APN);
    at_transact_locked(cmd, resp, sizeof(resp), 3000);

    ESP_LOGI(TAG, "selftest: activating PDP context (up to 30s)...");
    esp_err_t act = at_transact_locked("AT+QIACT=1", resp, sizeof(resp), 30000);
    bool act_ok = (act == ESP_OK) && (strstr(resp, "ERROR") == NULL);
    RPT("PDP activate: %s\n", act_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "selftest: PDP activate %s", act_ok ? "OK" : "FAIL");

    if (at_transact_locked("AT+QIACT?", resp, sizeof(resp), 5000) == ESP_OK) {
        char *first = strchr(resp, '"');
        if (first) {
            first++;
            size_t i = 0;
            while (*first && *first != '"' && i + 1 < sizeof(s_status.ip)) {
                s_status.ip[i++] = *first++;
            }
            s_status.ip[i] = '\0';
            if (i > 0) {
                s_status.ip_up = true;
            }
        }
    }
    RPT("IP: %s\n", s_status.ip[0] ? s_status.ip : "none");
    ESP_LOGI(TAG, "selftest: IP=%s", s_status.ip[0] ? s_status.ip : "none");

    if (act_ok) {
        ESP_LOGI(TAG, "selftest: pinging 8.8.8.8 (up to 9s)...");
        at_transact_locked("AT+QPING=1,\"8.8.8.8\",4,4", resp, sizeof(resp), 3000);
        char urc[256];
        at_collect_locked(urc, sizeof(urc), 9000);
        int replies = 0;
        for (const char *p = urc; (p = strstr(p, "+QPING: 0,")) != NULL; p += 9) {
            replies++;
        }
        RPT("Ping 8.8.8.8: %d reply line(s)\n", replies);
        if (replies > 0) {
            s_status.link_up = true;
            RPT("=> INTERNET OK over LTE\n");
        } else {
            RPT("=> no ping reply (data not confirmed)\n");
        }
        ESP_LOGI(TAG, "selftest: ping replies=%d", replies);
    }

done:
    xSemaphoreGive(s_uart_mutex);
    ESP_LOGI(TAG, "selftest: done (rc=%s)", esp_err_to_name(rc));
#undef RPT
    return rc;
}

esp_err_t net_lte_get_status(net_lte_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_status;
    return ESP_OK;
}

esp_err_t net_lte_reconnect(void)
{
    return net_lte_start();
}

#else /* !CONFIG_NET_LTE_ENABLE */

esp_err_t net_lte_start(void)
{
    ESP_LOGI(TAG, "LTE disabled (CONFIG_NET_LTE_ENABLE=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t net_lte_get_status(net_lte_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->last_error, "CONFIG_NET_LTE_ENABLE=n", sizeof(out->last_error) - 1);
    return ESP_OK;
}

esp_err_t net_lte_refresh(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t net_lte_selftest(char *report, size_t len)
{
    if (report && len) {
        snprintf(report, len, "LTE disabled (CONFIG_NET_LTE_ENABLE=n)");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t net_lte_reconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

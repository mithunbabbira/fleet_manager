#include "lte.h"
#include "lte_time.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/uart_private.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lte";

#if CONFIG_LTE_ENABLE

/* Modem can take several seconds to boot; retry AT for up to this long. */
#define LTE_AT_ATTEMPTS   60
#define LTE_AT_INTERVAL_MS 500

static lte_status_t s_status;
static bool s_uart_ready;
static SemaphoreHandle_t s_uart_mutex;
/* When true, gps_task skips AT so OTA QHTTP streams own the UART cleanly. */
static volatile bool s_suspend_bg_at;
static TaskHandle_t s_bringup_task;

/* Wall-clock cache: UTC epoch ms at sync + monotonic anchor for extrapolation. */
static SemaphoreHandle_t s_time_mutex;
static bool s_time_valid;
static int64_t s_time_epoch_ms_utc;
static int64_t s_time_mono_ms;
static lte_time_source_t s_time_source;
static int64_t s_last_cclk_query_ms;

#define LTE_CCLK_MIN_INTERVAL_MS (30 * 60 * 1000LL)
/* Retry sooner while the clock is still unset. */
#define LTE_CCLK_RETRY_MS        (60 * 1000LL)

#if CONFIG_LTE_GPS_ENABLE
/* GNSS cache, protected by its own mutex (independent of modem UART traffic
 * so lte_gps_get() never blocks behind an in-flight AT/HTTP transaction). */
static SemaphoreHandle_t s_gps_mutex;
static bool s_gps_ok;
static double s_gps_lat, s_gps_lng;
static int64_t s_gps_fix_ms; /* esp_timer ms when last good fix stored */
static TaskHandle_t s_gps_task;
static void gps_task(void *arg);
static void gps_task_start(void);
#endif

static esp_err_t at_transact(const char *cmd, char *resp, size_t resp_len, int timeout_ms);

/* Plausible window for a fielded device; rejects garbage AT parses. */
#define LTE_TIME_MIN_EPOCH_MS 1735689600000LL /* 2025-01-01 UTC */
#define LTE_TIME_MAX_EPOCH_MS 4102444800000LL /* 2100-01-01 UTC */

static void time_apply_sync_locked(int64_t epoch_ms_utc, lte_time_source_t source)
{
    if (epoch_ms_utc < LTE_TIME_MIN_EPOCH_MS || epoch_ms_utc > LTE_TIME_MAX_EPOCH_MS) {
        return;
    }
    /* GPS is unambiguous UTC; network CCLK depends on the operator reporting a
     * timezone. Once GPS has set the clock, don't let CCLK move it. */
    if (s_time_valid && s_time_source == LTE_TIME_GPS && source == LTE_TIME_CCLK) {
        return;
    }
    bool was_valid = s_time_valid;
    lte_time_source_t prev = s_time_source;
    s_time_valid = true;
    s_time_epoch_ms_utc = epoch_ms_utc;
    s_time_mono_ms = esp_timer_get_time() / 1000;
    s_time_source = source;
    if (!was_valid || prev != source) {
        ESP_LOGI(TAG, "time sync via %s", source == LTE_TIME_CCLK ? "CCLK" : "GPS");
    }
}

static void time_apply_sync(int64_t epoch_ms_utc, lte_time_source_t source)
{
    if (s_time_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    time_apply_sync_locked(epoch_ms_utc, source);
    xSemaphoreGive(s_time_mutex);
}

/**
 * @brief Parse +CCLK via shared util (IST-aware); one AT query — low UART load.
 */
static bool time_parse_cclk(const char *resp, int64_t *epoch_ms_utc_out)
{
    return lte_time_parse_cclk(resp, epoch_ms_utc_out);
}

/** @brief One AT+CCLK? query; true when a usable time was parsed. */
static bool time_try_cclk(void)
{
    char resp[96];
    if (at_transact("AT+CCLK?", resp, sizeof(resp), 2000) != ESP_OK) {
        return false;
    }
    int64_t epoch_ms = 0;
    if (!time_parse_cclk(resp, &epoch_ms)) {
        return false;
    }
    time_apply_sync(epoch_ms, LTE_TIME_CCLK);
    return true;
}

/**
 * @brief Query CCLK when due; honors OTA suspend. Cheap to call often.
 * @note An unregistered modem reports a 1980-ish date, which the plausibility
 *       check in time_apply_sync_locked rejects — no registration gate needed.
 */
static void time_maintain_cclk(void)
{
    if (s_suspend_bg_at) {
        return;
    }
    bool synced = false;
    if (s_time_mutex != NULL && xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        synced = s_time_valid;
        xSemaphoreGive(s_time_mutex);
    }
    int64_t mono = esp_timer_get_time() / 1000;
    int64_t due = synced ? LTE_CCLK_MIN_INTERVAL_MS : LTE_CCLK_RETRY_MS;
    if (s_last_cclk_query_ms != 0 && (mono - s_last_cclk_query_ms) < due) {
        return;
    }
    s_last_cclk_query_ms = mono;
    (void)time_try_cclk();
}

/** @brief Read epoch + source together so they can never disagree. */
static bool time_snapshot(uint64_t *epoch_ms_out, lte_time_source_t *source_out)
{
    if (s_time_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool ok = s_time_valid;
    int64_t now = 0;
    lte_time_source_t source = s_time_source;
    if (ok) {
        int64_t delta = (esp_timer_get_time() / 1000) - s_time_mono_ms;
        if (delta < 0) {
            delta = 0;
        }
        now = s_time_epoch_ms_utc + delta;
    }
    xSemaphoreGive(s_time_mutex);
    if (!ok || now <= 0) {
        return false;
    }
    if (epoch_ms_out) {
        *epoch_ms_out = (uint64_t)now;
    }
    if (source_out) {
        *source_out = source;
    }
    return true;
}

uint64_t lte_time_now_ms(void)
{
    uint64_t now = 0;
    return time_snapshot(&now, NULL) ? now : 0;
}

esp_err_t lte_time_get(lte_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    uint64_t now = 0;
    lte_time_source_t source = LTE_TIME_NONE;
    if (time_snapshot(&now, &source)) {
        out->time_ok = true;
        out->epoch_ms_utc = now;
        out->source = source;
    }
    return ESP_OK;
}

/**
 * @brief Raw AT write + collect until OK/ERROR/READY or timeout (caller holds UART mutex).
 * @note Drains stale UART bytes first. Returns ESP_OK when a terminator is seen (even ERROR).
 * @note Must finish the current line after seeing ERROR so "+CME ERROR: 504" is not truncated
 *       mid-token (substring "ERROR" appears before ": 504" arrives on the wire).
 */
static esp_err_t at_transact_locked(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    if (resp && resp_len) {
        resp[0] = '\0';
    }

    /* Drain stale bytes. */
    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }

    char line[96];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    if (n <= 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(CONFIG_LTE_UART_PORT, line, n);
    if (written != n) {
        return ESP_FAIL;
    }

    size_t used = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool saw_result = false;
    TickType_t line_grace = 0;
    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
        if (got <= 0) {
            if (saw_result && xTaskGetTickCount() >= line_grace) {
                return ESP_OK;
            }
            continue;
        }
        if (resp && used + 1 < resp_len) {
            resp[used++] = (char)ch;
            resp[used] = '\0';
        }
        if (!saw_result && used >= 2) {
            if (strstr(resp, "OK") != NULL || strstr(resp, "ERROR") != NULL ||
                strstr(resp, "READY") != NULL) {
                saw_result = true;
                /* Allow the rest of the line (e.g. ": 504\r\n") to arrive. */
                line_grace = xTaskGetTickCount() + pdMS_TO_TICKS(80);
            }
        }
        if (saw_result && ch == '\n') {
            return ESP_OK;
        }
    }
    return saw_result ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief Take UART mutex then at_transact_locked; timeout includes mutex wait slack.
 */
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

/**
 * @brief Install/configure UART1 once (large RX buffer for QHTTP bodies).
 */
static esp_err_t uart_init(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

    uart_config_t cfg = {
        .baud_rate = CONFIG_LTE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* ESP32-C6: Zigbee/coex can leave UART1 core clock off; driver install hangs without this. */
    if (CONFIG_LTE_UART_PORT < SOC_UART_HP_NUM) {
        HP_UART_BUS_CLK_ATOMIC() {
            uart_ll_enable_bus_clock(CONFIG_LTE_UART_PORT, true);
        }
        HP_UART_SRC_CLK_ATOMIC() {
            uart_dev_t *hw = UART_LL_GET_HW(CONFIG_LTE_UART_PORT);
            uart_ll_sclk_enable(hw);
            uart_ll_set_sclk(hw, UART_SCLK_DEFAULT);
        }
    }

    esp_err_t err = uart_driver_install(CONFIG_LTE_UART_PORT, 16384, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = uart_param_config(CONFIG_LTE_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(CONFIG_LTE_UART_PORT,
                       CONFIG_LTE_UART_TX_GPIO,
                       CONFIG_LTE_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    s_uart_ready = true;
    ESP_LOGI(TAG, "UART%d ready TX=GPIO%d RX=GPIO%d baud=%d",
             CONFIG_LTE_UART_PORT,
             CONFIG_LTE_UART_TX_GPIO,
             CONFIG_LTE_UART_RX_GPIO,
             CONFIG_LTE_UART_BAUD);
    return ESP_OK;
}

/** @brief Background: retry AT/ATI then refresh registration; deletes self when done. */
static void bringup_task(void *arg)
{
    (void)arg;
    char resp[256];

    strncpy(s_status.last_error, "probing modem AT (waiting for boot)...",
            sizeof(s_status.last_error) - 1);

    bool at_ok = false;
    for (int attempt = 1; attempt <= LTE_AT_ATTEMPTS; ++attempt) {
        if (at_transact("AT", resp, sizeof(resp), 1000) == ESP_OK && strstr(resp, "OK")) {
            at_ok = true;
            ESP_LOGI(TAG, "AT OK after %d attempt(s) (wiring looks good)", attempt);
            break;
        }
        if (attempt == 1 || attempt % 4 == 0) {
            ESP_LOGW(TAG, "no AT yet (attempt %d/%d); modem may still be booting",
                     attempt, LTE_AT_ATTEMPTS);
        }
        vTaskDelay(pdMS_TO_TICKS(LTE_AT_INTERVAL_MS));
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

#if CONFIG_LTE_GPS_ENABLE
    gps_task_start();
#endif

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

    strncpy(s_status.last_error, "UART AT OK; HTTP via QHTTP (no PPP)",
            sizeof(s_status.last_error) - 1);
    ESP_LOGI(TAG, "modem UART ready — uplink/OTA use Quectel QHTTP AT commands");

    lte_refresh();
    ESP_LOGI(TAG, "modem: sim_ready=%d reg=%d attached=%d csq=%d op='%s'",
             s_status.sim_ready, s_status.registered, s_status.attached,
             s_status.csq, s_status.operator_name);

    s_bringup_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t lte_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = true;
    strncpy(s_status.apn, CONFIG_LTE_APN, sizeof(s_status.apn) - 1);

    if (s_uart_mutex == NULL) {
        s_uart_mutex = xSemaphoreCreateMutex();
        if (s_uart_mutex == NULL) {
            strncpy(s_status.last_error, "mutex alloc failed", sizeof(s_status.last_error) - 1);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_time_mutex == NULL) {
        s_time_mutex = xSemaphoreCreateMutex();
        if (s_time_mutex == NULL) {
            strncpy(s_status.last_error, "time mutex alloc failed", sizeof(s_status.last_error) - 1);
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
             (LTE_AT_ATTEMPTS * LTE_AT_INTERVAL_MS) / 1000);
    return ESP_OK;
}

/**
 * @brief Parse +CSQ into s_status.csq and rssi_dbm.
 */
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

/**
 * @brief Parse quoted operator name from +COPS.
 */
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

esp_err_t lte_refresh(void)
{
    if (!s_uart_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_suspend_bg_at) {
        /* OTA owns the UART for minutes — don't queue AT behind it. */
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
    time_maintain_cclk();
    return ESP_OK;
}

void lte_suspend_bg_at(bool suspend)
{
    s_suspend_bg_at = suspend;
    if (suspend) {
        ESP_LOGI(TAG, "background AT suspended (OTA)");
    } else {
        ESP_LOGI(TAG, "background AT resumed");
    }
}

#if CONFIG_LTE_GPS_ENABLE
/*
 * Parse "+QGPSLOC: <utc>,<lat>,<lng>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,
 * <date>,<nsat>" as returned by `AT+QGPSLOC=2` (decimal-degree mode) on the
 * EC200U. Field 0 is UTC (HHMMSS.ss), field 9 is date (DDMMYY).
 * Verified against Quectel EC200U/EG915U GNSS application-note examples;
 * adjust indices here if a different firmware/URC layout is seen in the field.
 */
static bool parse_qgpsloc(const char *resp, double *lat_out, double *lng_out)
{
    const char *p = strstr(resp, "+QGPSLOC:");
    if (p == NULL) {
        return false;
    }
    p += strlen("+QGPSLOC:");

    double lat = 0.0, lng = 0.0;
    if (sscanf(p, " %*[^,],%lf,%lf", &lat, &lng) != 2) {
        return false;
    }
    if (lat_out) {
        *lat_out = lat;
    }
    if (lng_out) {
        *lng_out = lng;
    }

    /* Best-effort wall clock from the same reply; never affects the fix result. */
    int hh = 0;
    int mi = 0;
    int ss = 0;
    if (sscanf(p, " %2d%2d%2d", &hh, &mi, &ss) == 3) {
        const char *date_p = p;
        for (int field = 0; field < 9 && date_p != NULL; field++) {
            date_p = strchr(date_p, ',');
            if (date_p != NULL) {
                date_p++;
            }
        }
        int dd = 0;
        int mo = 0;
        int yy = 0;
        if (date_p != NULL && sscanf(date_p, "%2d%2d%2d", &dd, &mo, &yy) == 3) {
            int64_t epoch_ms = lte_utc_datetime_to_epoch_ms(yy + 2000, mo, dd, hh, mi, ss);
            if (epoch_ms >= 0) {
                time_apply_sync(epoch_ms, LTE_TIME_GPS);
            }
        }
    }
    return true;
}

/* Modem bring-up (AT probing in bringup_task) can take up to ~20s, so a
 * single QGPS=1 fired right after UART init routinely lands before the
 * modem is ready and is silently discarded, leaving GNSS off forever.
 * Retry with backoff for a while before giving up. */
#define LTE_GPS_ENABLE_ATTEMPTS        40
#define LTE_GPS_ENABLE_BACKOFF_MS      500
#define LTE_GPS_ENABLE_BACKOFF_MAX_MS  5000

/* True if AT+QGPS=1 reply means GNSS is (now) on. */
static bool gps_qgps_reply_ok(const char *resp)
{
    if (resp == NULL || resp[0] == '\0') {
        return false;
    }
    /* Already started — do not tear down. */
    if (strstr(resp, "504") != NULL) {
        return true;
    }
    return strstr(resp, "OK") != NULL && strstr(resp, "ERROR") == NULL;
}

/* Attempt to turn GNSS on once. Returns true if the modem accepted it, or
 * if it reports a session already active (+CME ERROR: 504 on the EC200U).
 * Only issues QGPSEND when start fails with a non-504 error (stuck session). */
static bool gps_enable_once(char *resp, size_t resp_len)
{
    esp_err_t err = at_transact("AT+QGPS=1", resp, resp_len, 5000);
    if (err == ESP_OK && gps_qgps_reply_ok(resp)) {
        return true;
    }

    /* Clear a stuck session, then start again. */
    (void)at_transact("AT+QGPSEND", resp, resp_len, 2000);
    vTaskDelay(pdMS_TO_TICKS(300));
    err = at_transact("AT+QGPS=1", resp, resp_len, 5000);
    return err == ESP_OK && gps_qgps_reply_ok(resp);
}

static void gps_task_start(void)
{
    if (s_gps_mutex == NULL) {
        s_gps_mutex = xSemaphoreCreateMutex();
    }
    if (s_gps_mutex != NULL && s_gps_task == NULL) {
        if (xTaskCreate(gps_task, "lte_gps", 4096, NULL, 3, &s_gps_task) != pdPASS) {
            ESP_LOGW(TAG, "gps task spawn failed; lat/lng will stay unavailable");
            s_gps_task = NULL;
        }
    }
}

/**
 * @brief Enable GNSS with backoff, then poll QGPSLOC; honors s_suspend_bg_at.
 * @note Keeps retrying enable in the poll loop if the first burst fails.
 */
static void gps_task(void *arg)
{
    (void)arg;
    char resp[192];

    while (!s_uart_ready) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Let bring-up / CPIN / attach settle before first GNSS attempt. */
    vTaskDelay(pdMS_TO_TICKS(8000));
    (void)at_transact("AT+CMEE=2", resp, sizeof(resp), 1000);

    bool gps_enabled = false;
    int backoff_ms = LTE_GPS_ENABLE_BACKOFF_MS;
    for (int attempt = 1; attempt <= LTE_GPS_ENABLE_ATTEMPTS; ++attempt) {
        while (s_suspend_bg_at) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (gps_enable_once(resp, sizeof(resp))) {
            gps_enabled = true;
            ESP_LOGI(TAG, "AT+QGPS=1 accepted after %d attempt(s): %s", attempt,
                     resp[0] ? resp : "OK");
            break;
        }
        if (attempt == 1 || attempt % 8 == 0) {
            ESP_LOGW(TAG, "AT+QGPS=1 failed (attempt %d/%d): %s", attempt,
                     LTE_GPS_ENABLE_ATTEMPTS, resp[0] ? resp : "no response");
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms * 2 > LTE_GPS_ENABLE_BACKOFF_MAX_MS)
                         ? LTE_GPS_ENABLE_BACKOFF_MAX_MS
                         : backoff_ms * 2;
    }
    if (!gps_enabled) {
        ESP_LOGW(TAG, "AT+QGPS=1 not accepted yet after %d attempts; will keep retrying",
                 LTE_GPS_ENABLE_ATTEMPTS);
    }

    for (;;) {
        while (s_suspend_bg_at) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!gps_enabled) {
            if (gps_enable_once(resp, sizeof(resp))) {
                gps_enabled = true;
                ESP_LOGI(TAG, "AT+QGPS=1 accepted (retry): %s", resp[0] ? resp : "OK");
            }
        }
        if (at_transact("AT+QGPSLOC=2", resp, sizeof(resp), 3000) == ESP_OK) {
            double lat = 0.0, lng = 0.0;
            if (parse_qgpsloc(resp, &lat, &lng)) {
                gps_enabled = true;
                if (xSemaphoreTake(s_gps_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    s_gps_ok = true;
                    s_gps_lat = lat;
                    s_gps_lng = lng;
                    s_gps_fix_ms = esp_timer_get_time() / 1000;
                    xSemaphoreGive(s_gps_mutex);
                }
            } else if (strstr(resp, "505") != NULL || strstr(resp, "516") != NULL) {
                ESP_LOGW(TAG, "GNSS inactive/busy (%s); re-enabling", resp);
                gps_enabled = gps_enable_once(resp, sizeof(resp));
            }
        }
        /* Self-throttled; keeps the wall clock alive indoors with no fix. */
        time_maintain_cclk();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LTE_GPS_REFRESH_S * 1000));
    }
}
#endif /* CONFIG_LTE_GPS_ENABLE */

esp_err_t lte_gps_get(lte_gps_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if CONFIG_LTE_GPS_ENABLE
    if (s_gps_mutex == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_gps_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool ok = s_gps_ok;
    double lat = s_gps_lat;
    double lng = s_gps_lng;
    int64_t fix_ms = s_gps_fix_ms;
    xSemaphoreGive(s_gps_mutex);

    int64_t age_ms = (esp_timer_get_time() / 1000) - fix_ms;
    if (!ok || age_ms < 0 || age_ms > (int64_t)CONFIG_LTE_GPS_MAX_AGE_S * 1000) {
        out->gps_ok = false;
        return ESP_OK;
    }
    out->gps_ok = true;
    out->lat = lat;
    out->lng = lng;
    out->age_ms = (uint32_t)age_ms;
#endif
    return ESP_OK;
}

/**
 * @brief Collect UART bytes for a fixed window (no token); used for URC tails / ping.
 */
static esp_err_t at_collect_locked(char *resp, size_t resp_len, int window_ms)
{
    size_t used = 0;
    if (resp && resp_len) {
        resp[0] = '\0';
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(window_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
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

esp_err_t lte_selftest(char *report, size_t len)
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
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",0", CONFIG_LTE_APN);
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

esp_err_t lte_get_status(lte_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_status;
    return ESP_OK;
}

esp_err_t lte_reconnect(void)
{
    return lte_start();
}

/**
 * @brief Wait until @p token or ERROR appears in the response buffer.
 */
static esp_err_t at_wait_token_locked(char *resp, size_t resp_len, int timeout_ms,
                                      const char *token)
{
    size_t used = 0;
    if (resp && resp_len) {
        resp[0] = '\0';
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        if (resp && used + 1 < resp_len) {
            resp[used++] = (char)ch;
            resp[used] = '\0';
        }
        if (resp && token && strstr(resp, token) != NULL) {
            return ESP_OK;
        }
        if (resp && strstr(resp, "ERROR") != NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Parse +QIACT? response into s_status.ip; set ip_up/link_up when IP found.
 * @return true if a non-empty IP was captured.
 */
static bool pdp_parse_ip_locked(char *resp, size_t resp_len)
{
    if (at_transact_locked("AT+QIACT?", resp, resp_len, 5000) != ESP_OK) {
        return false;
    }
    char *first = strchr(resp, '"');
    if (!first) {
        return false;
    }
    first++;
    size_t i = 0;
    while (*first && *first != '"' && i + 1 < sizeof(s_status.ip)) {
        s_status.ip[i++] = *first++;
    }
    s_status.ip[i] = '\0';
    if (i == 0) {
        return false;
    }
    s_status.ip_up = true;
    s_status.link_up = true;
    return true;
}

/**
 * @brief Ensure PDP: QICSGP + activate context; recover with QIDEACT if stuck.
 *
 * If QIACT? already has an IP, skip QIACT=1 (avoids noisy ERROR when already up).
 * Otherwise activate; on failure deactivate once and retry activate.
 *
 * @return ESP_OK if IP present; ESP_FAIL otherwise.
 */
static esp_err_t ensure_pdp_locked(char *resp, size_t resp_len)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",0", CONFIG_LTE_APN);
    at_transact_locked(cmd, resp, resp_len, 3000);

    if (pdp_parse_ip_locked(resp, resp_len)) {
        return ESP_OK;
    }

    esp_err_t act = at_transact_locked("AT+QIACT=1", resp, resp_len, 30000);
    if (act == ESP_OK && strstr(resp, "ERROR") == NULL) {
        if (pdp_parse_ip_locked(resp, resp_len)) {
            return ESP_OK;
        }
    } else if (strstr(resp, "ERROR") != NULL) {
        ESP_LOGW(TAG, "QIACT activate failed, trying QIDEACT+retry");
    }

    at_transact_locked("AT+QIDEACT=1", resp, resp_len, 15000);
    vTaskDelay(pdMS_TO_TICKS(500));
    act = at_transact_locked("AT+QIACT=1", resp, resp_len, 30000);
    if (act != ESP_OK || strstr(resp, "ERROR") != NULL) {
        ESP_LOGW(TAG, "QIACT retry: %s", resp);
    }
    if (pdp_parse_ip_locked(resp, resp_len)) {
        return ESP_OK;
    }
    return s_status.ip_up ? ESP_OK : ESP_FAIL;
}

/**
 * @brief QHTTPURL length handshake: wait CONNECT, write URL, wait OK.
 */
static esp_err_t http_set_url_locked(const char *url, char *resp, size_t resp_len)
{
    size_t url_len = strlen(url);
    char cmd[48];

    /* Drain then send URL length command; wait for CONNECT. */
    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }
    int n = snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,80\r", (unsigned)url_len);
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uart_write_bytes(CONFIG_LTE_UART_PORT, cmd, n) != n) {
        return ESP_FAIL;
    }
    if (at_wait_token_locked(resp, resp_len, 80000, "CONNECT") != ESP_OK) {
        return ESP_FAIL;
    }
    if ((size_t)uart_write_bytes(CONFIG_LTE_UART_PORT, url, url_len) != url_len) {
        return ESP_FAIL;
    }
    return at_wait_token_locked(resp, resp_len, 80000, "OK");
}

esp_err_t lte_http_post(const char *url, const char *body, lte_http_result_t *out)
{
    return lte_http_post_recv(url, body, NULL, NULL, 0, NULL, out);
}

/**
 * @brief Shared GET setup: PDP, SSL/HTTP cfg, URL, QHTTPGET URC → status/content-length.
 */
static esp_err_t http_prepare_get_locked(const char *url, char *resp, size_t resp_len,
                                         int *http_status, size_t *content_len)
{
    *http_status = 0;
    *content_len = 0;

    if (at_transact_locked("AT", resp, resp_len, 1000) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        return ESP_FAIL;
    }
    if (ensure_pdp_locked(resp, resp_len) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    at_transact_locked("AT+QHTTPCFG=\"contextid\",1", resp, resp_len, 3000);
    at_transact_locked("AT+QHTTPCFG=\"sslctxid\",1", resp, resp_len, 3000);
    at_transact_locked("AT+QSSLCFG=\"sslversion\",1,4", resp, resp_len, 3000);
    at_transact_locked("AT+QSSLCFG=\"seclevel\",1,0", resp, resp_len, 3000);
    at_transact_locked("AT+QSSLCFG=\"sni\",1,1", resp, resp_len, 3000);
    at_transact_locked("AT+QHTTPCFG=\"requestheader\",0", resp, resp_len, 3000);
    at_transact_locked("AT+QHTTPCFG=\"responseheader\",0", resp, resp_len, 3000);

    if (http_set_url_locked(url, resp, resp_len) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }
    if (at_transact_locked("AT+QHTTPGET=120", resp, resp_len, 5000) != ESP_OK) {
        /* OK may arrive before URC; still wait for URC below */
    }

    char urc[384];
    at_wait_token_locked(urc, sizeof(urc), 180000, "+QHTTPGET:");
    {
        char tail[64];
        at_collect_locked(tail, sizeof(tail), 300);
        size_t used = strlen(urc);
        snprintf(urc + used, sizeof(urc) - used, "%s", tail);
    }
    const char *p = strstr(urc, "+QHTTPGET:");
    if (p == NULL) {
        at_collect_locked(urc, sizeof(urc), 5000);
        p = strstr(urc, "+QHTTPGET:");
    }
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    int err = -1;
    int status = 0;
    int rlen = 0;
    int n = sscanf(p, "+QHTTPGET: %d,%d,%d", &err, &status, &rlen);
    if (n < 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *http_status = status;
    if (n >= 3 && rlen > 0) {
        *content_len = (size_t)rlen;
    }
    if (err != 0 || status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief After CONNECT, stream body to callback; skip leading CRLF so CL matches.
 * @note Known content_len path is strict; unknown length uses idle timeout.
 */
static esp_err_t http_read_body_stream_locked(size_t content_len, lte_http_chunk_cb_t cb,
                                              void *ctx, char *scratch, size_t scratch_len)
{
    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }

    const char *cmd = "AT+QHTTPREAD=120\r";
    if (uart_write_bytes(CONFIG_LTE_UART_PORT, cmd, strlen(cmd)) != (int)strlen(cmd)) {
        return ESP_FAIL;
    }
    if (at_wait_token_locked(scratch, scratch_len, 120000, "CONNECT") != ESP_OK) {
        return ESP_FAIL;
    }

    /* Quectel emits CONNECT\r\n then the HTTP body. Do not count CRLF as body bytes
     * or a known Content-Length JSON payload gets truncated and cJSON fails. */
    uint8_t first = 0;
    bool have_first = false;
    for (int i = 0; i < 8; ++i) {
        uint8_t ch;
        int n = uart_read_bytes(CONFIG_LTE_UART_PORT, &ch, 1, pdMS_TO_TICKS(500));
        if (n <= 0) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        first = ch;
        have_first = true;
        break;
    }

    uint8_t chunk[1024];
    size_t chunk_used = 0;
    if (have_first) {
        chunk[0] = first;
        chunk_used = 1;
    }

    if (content_len > 0) {
        size_t remaining = content_len;
        if (chunk_used > remaining) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* Emit the already-read first byte as part of the stream. */
        while (remaining > 0) {
            size_t want = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
            size_t got = chunk_used;
            chunk_used = 0;
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
            while (got < want) {
                if (xTaskGetTickCount() > deadline) {
                    return ESP_ERR_TIMEOUT;
                }
                int n = uart_read_bytes(CONFIG_LTE_UART_PORT, chunk + got, want - got,
                                        pdMS_TO_TICKS(200));
                if (n > 0) {
                    got += (size_t)n;
                    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
                }
            }
            if (cb) {
                esp_err_t cer = cb(chunk, got, ctx);
                if (cer != ESP_OK) {
                    return cer;
                }
            }
            remaining -= got;
        }
    } else {
        size_t total = chunk_used;
        if (chunk_used && cb) {
            esp_err_t cer = cb(chunk, chunk_used, ctx);
            if (cer != ESP_OK) {
                return cer;
            }
            chunk_used = 0;
        }
        TickType_t idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        for (;;) {
            int n = uart_read_bytes(CONFIG_LTE_UART_PORT, chunk, sizeof(chunk),
                                    pdMS_TO_TICKS(200));
            if (n > 0) {
                if (cb) {
                    esp_err_t cer = cb(chunk, (size_t)n, ctx);
                    if (cer != ESP_OK) {
                        return cer;
                    }
                }
                total += (size_t)n;
                idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                continue;
            }
            if (xTaskGetTickCount() > idle_deadline) {
                break;
            }
        }
        (void)total;
    }

    at_collect_locked(scratch, scratch_len, 3000);
    return ESP_OK;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t used;
} http_get_buf_ctx_t;

/**
 * @brief Chunk callback that appends into a fixed buffer (NUL-terminated).
 */
static esp_err_t http_get_buf_cb(const uint8_t *data, size_t len, void *ctx)
{
    http_get_buf_ctx_t *c = (http_get_buf_ctx_t *)ctx;
    if (c->used + len >= c->cap) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(c->buf + c->used, data, len);
    c->used += len;
    c->buf[c->used] = '\0';
    return ESP_OK;
}

/**
 * @brief Split http(s)://host/path?query into host and path_and_query.
 */
static esp_err_t http_parse_url_host_path(const char *url, char *host, size_t host_len,
                                          char *path, size_t path_len)
{
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= host_len) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (slash == NULL) {
        if (path_len < 2) {
            return ESP_ERR_INVALID_ARG;
        }
        path[0] = '/';
        path[1] = '\0';
        return ESP_OK;
    }
    if (strlen(slash) >= path_len) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(path, slash, path_len - 1);
    path[path_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t lte_http_post_recv(const char *url, const char *body,
                                 const lte_http_req_headers_t *hdr,
                                 char *resp_buf, size_t resp_buf_len, size_t *resp_len,
                                 lte_http_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (resp_len) {
        *resp_len = 0;
    }
    if (url == NULL || body == NULL || url[0] == '\0') {
        if (out) {
            snprintf(out->error, sizeof(out->error), "invalid args");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (resp_buf != NULL && resp_buf_len < 2) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "invalid args");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_uart_ready || s_uart_mutex == NULL) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "UART not ready");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(120000)) != pdTRUE) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "modem busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    const bool use_hdr = hdr &&
                         ((hdr->authorization && hdr->authorization[0]) ||
                          (hdr->system_user_id && hdr->system_user_id[0]));

    char resp[512];
    esp_err_t rc = ESP_FAIL;
    char *payload = NULL;
    size_t payload_len = 0;

    if (at_transact_locked("AT", resp, sizeof(resp), 1000) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "AT fail");
        }
        goto done;
    }

    if (ensure_pdp_locked(resp, sizeof(resp)) != ESP_OK) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "PDP/IP fail");
        }
        goto done;
    }

    at_transact_locked("AT+QHTTPCFG=\"contextid\",1", resp, sizeof(resp), 3000);
    at_transact_locked("AT+QHTTPCFG=\"sslctxid\",1", resp, sizeof(resp), 3000);
    at_transact_locked("AT+QSSLCFG=\"sslversion\",1,4", resp, sizeof(resp), 3000);
    at_transact_locked("AT+QSSLCFG=\"seclevel\",1,0", resp, sizeof(resp), 3000);
    at_transact_locked("AT+QSSLCFG=\"sni\",1,1", resp, sizeof(resp), 3000);

    if (use_hdr) {
        at_transact_locked("AT+QHTTPCFG=\"requestheader\",1", resp, sizeof(resp), 3000);
    } else {
        at_transact_locked("AT+QHTTPCFG=\"requestheader\",0", resp, sizeof(resp), 3000);
        /* EC200U content types: 0=urlencoded 1=text/plain 2=octet-stream
         * 3=multipart 4=application/json (1 caused HTTP 415 from the API). */
        at_transact_locked("AT+QHTTPCFG=\"contenttype\",4", resp, sizeof(resp), 3000);
    }
    at_transact_locked("AT+QHTTPCFG=\"responseheader\",0", resp, sizeof(resp), 3000);

    if (http_set_url_locked(url, resp, sizeof(resp)) != ESP_OK) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "QHTTPURL fail");
        }
        goto done;
    }

    size_t body_len = strlen(body);
    const char *tx = body;
    size_t tx_len = body_len;

    if (use_hdr) {
        char host[128];
        char path[384];
        if (http_parse_url_host_path(url, host, sizeof(host), path, sizeof(path)) != ESP_OK) {
            if (out) {
                snprintf(out->error, sizeof(out->error), "URL parse fail");
            }
            goto done;
        }
        size_t cap = body_len + strlen(host) + strlen(path) + 512;
        payload = (char *)malloc(cap);
        if (payload == NULL) {
            if (out) {
                snprintf(out->error, sizeof(out->error), "payload OOM");
            }
            rc = ESP_ERR_NO_MEM;
            goto done;
        }
        int n = snprintf(payload, cap,
                         "POST %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %u\r\n",
                         path, host, (unsigned)body_len);
        if (n < 0 || (size_t)n >= cap) {
            if (out) {
                snprintf(out->error, sizeof(out->error), "payload build fail");
            }
            goto done;
        }
        if (hdr->authorization && hdr->authorization[0]) {
            int a = snprintf(payload + n, cap - (size_t)n, "Authorization: %s\r\n",
                             hdr->authorization);
            if (a < 0 || (size_t)a >= cap - (size_t)n) {
                if (out) {
                    snprintf(out->error, sizeof(out->error), "payload build fail");
                }
                goto done;
            }
            n += a;
        }
        if (hdr->system_user_id && hdr->system_user_id[0]) {
            int s = snprintf(payload + n, cap - (size_t)n, "x-nc-system-user-id: %s\r\n",
                             hdr->system_user_id);
            if (s < 0 || (size_t)s >= cap - (size_t)n) {
                if (out) {
                    snprintf(out->error, sizeof(out->error), "payload build fail");
                }
                goto done;
            }
            n += s;
        }
        if ((size_t)n + 2 + body_len >= cap) {
            if (out) {
                snprintf(out->error, sizeof(out->error), "payload build fail");
            }
            goto done;
        }
        payload[n++] = '\r';
        payload[n++] = '\n';
        memcpy(payload + n, body, body_len);
        n += (int)body_len;
        payload[n] = '\0';
        payload_len = (size_t)n;
        tx = payload;
        tx_len = payload_len;
    }

    char post_cmd[48];
    int pn = snprintf(post_cmd, sizeof(post_cmd), "AT+QHTTPPOST=%u,80,80\r",
                      (unsigned)tx_len);
    uint8_t drain[64];
    while (uart_read_bytes(CONFIG_LTE_UART_PORT, drain, sizeof(drain), 0) > 0) {
    }
    if (uart_write_bytes(CONFIG_LTE_UART_PORT, post_cmd, pn) != pn) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "QHTTPPOST write fail");
        }
        goto done;
    }
    if (at_wait_token_locked(resp, sizeof(resp), 80000, "CONNECT") != ESP_OK) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "QHTTPPOST no CONNECT");
        }
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if ((size_t)uart_write_bytes(CONFIG_LTE_UART_PORT, tx, tx_len) != tx_len) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "body write fail");
        }
        goto done;
    }

    /* Wait for the +QHTTPPOST URC (arrives in seconds; don't burn the full
     * window like a fixed collect would), then grab the status digits that
     * follow the token on the same line. */
    char urc[384];
    at_wait_token_locked(urc, sizeof(urc), 90000, "+QHTTPPOST:");
    {
        char tail[64];
        at_collect_locked(tail, sizeof(tail), 300);
        size_t used = strlen(urc);
        snprintf(urc + used, sizeof(urc) - used, "%s", tail);
    }
    const char *p = strstr(urc, "+QHTTPPOST:");
    if (p == NULL) {
        p = strstr(resp, "+QHTTPPOST:");
    }
    if (p == NULL) {
        /* Sometimes status arrives late after OK — one more short collect. */
        at_collect_locked(urc, sizeof(urc), 5000);
        p = strstr(urc, "+QHTTPPOST:");
    }
    if (p == NULL) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "no QHTTPPOST URC");
        }
        goto done;
    }

    int err = -1;
    int status = 0;
    int rlen = 0;
    if (sscanf(p, "+QHTTPPOST: %d,%d,%d", &err, &status, &rlen) < 2) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "bad QHTTPPOST parse");
        }
        goto done;
    }
    if (out) {
        out->http_status = status;
    }
    if (err == 0 && status >= 200 && status < 300) {
        rc = ESP_OK;
        strncpy(s_status.last_error, "HTTP POST OK", sizeof(s_status.last_error) - 1);
        if (out) {
            out->error[0] = '\0';
        }
    } else {
        if (out) {
            snprintf(out->error, sizeof(out->error), "HTTP err=%d status=%d", err, status);
        }
        snprintf(s_status.last_error, sizeof(s_status.last_error),
                 "HTTP POST fail status=%d", status);
        rc = ESP_FAIL;
    }

    if (resp_buf != NULL) {
        size_t clen = (rlen > 0) ? (size_t)rlen : 0;
        /* Stream when body length is known or HTTP 2xx; otherwise short-drain
         * (non-2xx with rlen==0 can hang 120s waiting for CONNECT). */
        const bool stream_body = (rlen > 0) || (rc == ESP_OK);
        if (stream_body) {
            if (clen > 0 && clen >= resp_buf_len) {
                if (out) {
                    snprintf(out->error, sizeof(out->error), "response too large (%u)",
                             (unsigned)clen);
                }
                rc = ESP_ERR_NO_MEM;
                /* Drain unread QHTTP body so the next LTE HTTP call is not stuck. */
                at_transact_locked("AT+QHTTPREAD=80", resp, sizeof(resp), 5000);
            } else {
                http_get_buf_ctx_t bctx = {.buf = resp_buf, .cap = resp_buf_len, .used = 0};
                resp_buf[0] = '\0';
                esp_err_t read_rc =
                    http_read_body_stream_locked(clen, http_get_buf_cb, &bctx, resp, sizeof(resp));
                if (read_rc == ESP_OK) {
                    if (resp_len) {
                        *resp_len = bctx.used;
                    }
                } else {
                    if (out && (rc == ESP_OK || out->error[0] == '\0')) {
                        snprintf(out->error, sizeof(out->error), "QHTTPREAD fail");
                    }
                    if (rc == ESP_OK) {
                        rc = read_rc;
                    }
                }
            }
        } else {
            at_transact_locked("AT+QHTTPREAD=80", resp, sizeof(resp), 5000);
        }
    } else {
        /* Best-effort drain response body so next call starts clean. */
        at_transact_locked("AT+QHTTPREAD=80", resp, sizeof(resp), 5000);
    }

done:
    free(payload);
    xSemaphoreGive(s_uart_mutex);
    ESP_LOGI(TAG, "http_post_recv rc=%s status=%d len=%u", esp_err_to_name(rc),
             out ? out->http_status : -1, (unsigned)(resp_len ? *resp_len : 0));
    return rc;
}

/*
 * HTTPS GET helper (buffering version).
 *
 * Used by fw_ota_lte to fetch the *manifest JSON*:
 * - We run QHTTPGET and then read the response into `buf`.
 * - Callers provide a small buffer (MANIFEST_BUF_LEN) so we don't
 *   risk large RAM usage.
 *
 * Concurrency note:
 * - All modem AT transactions are serialized with `s_uart_mutex`.
 * - That prevents "response mixing" when multiple tasks (OTA + telemetry)
 *   talk to the modem over the same UART.
 */
esp_err_t lte_http_get(const char *url, char *buf, size_t buf_len, size_t *out_len,
                          lte_http_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!url || !buf || buf_len < 2) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "invalid args");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_uart_ready || s_uart_mutex == NULL) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "UART not ready");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(300000)) != pdTRUE) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "modem busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    char resp[512];
    int status = 0;
    size_t clen = 0;
    esp_err_t rc = http_prepare_get_locked(url, resp, sizeof(resp), &status, &clen);
    if (out) {
        out->http_status = status;
    }
    if (rc != ESP_OK) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "QHTTPGET fail status=%d", status);
        }
        goto done;
    }
    if (clen > 0 && clen >= buf_len) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "response too large (%u)", (unsigned)clen);
        }
        rc = ESP_ERR_NO_MEM;
        goto done;
    }

    http_get_buf_ctx_t bctx = {.buf = buf, .cap = buf_len, .used = 0};
    buf[0] = '\0';
    rc = http_read_body_stream_locked(clen, http_get_buf_cb, &bctx, resp, sizeof(resp));
    if (rc == ESP_OK) {
        if (out_len) {
            *out_len = bctx.used;
        }
        if (out) {
            out->error[0] = '\0';
        }
    } else if (out) {
        snprintf(out->error, sizeof(out->error), "QHTTPREAD fail");
    }

done:
    xSemaphoreGive(s_uart_mutex);
    ESP_LOGI(TAG, "http_get rc=%s status=%d len=%u", esp_err_to_name(rc),
             out ? out->http_status : -1, (unsigned)(out_len ? *out_len : 0));
    return rc;
}

/*
 * HTTPS GET helper (streaming version).
 *
 * Used by fw_ota_lte to fetch the *firmware .bin*:
 * - We run QHTTPGET to set up the download.
 * - We read the body via QHTTPREAD in chunks and invoke `cb(chunk,len,ctx)`.
 * - We do not buffer the full binary in RAM.
 */
esp_err_t lte_http_get_stream(const char *url, lte_http_chunk_cb_t cb, void *ctx,
                                  size_t *content_length_out, lte_http_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (content_length_out) {
        *content_length_out = 0;
    }
    if (!url || !cb) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "invalid args");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_uart_ready || s_uart_mutex == NULL) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "UART not ready");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(300000)) != pdTRUE) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "modem busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    char resp[512];
    int status = 0;
    size_t clen = 0;
    esp_err_t rc = http_prepare_get_locked(url, resp, sizeof(resp), &status, &clen);
    if (out) {
        out->http_status = status;
    }
    if (content_length_out) {
        *content_length_out = clen;
    }
    if (rc != ESP_OK) {
        if (out) {
            snprintf(out->error, sizeof(out->error), "QHTTPGET fail status=%d", status);
        }
        goto done;
    }

    rc = http_read_body_stream_locked(clen, cb, ctx, resp, sizeof(resp));
    if (rc == ESP_OK) {
        if (out) {
            out->error[0] = '\0';
        }
    } else if (out && out->error[0] == '\0') {
        snprintf(out->error, sizeof(out->error), "stream read fail");
    }

done:
    xSemaphoreGive(s_uart_mutex);
    ESP_LOGI(TAG, "http_get_stream rc=%s status=%d clen=%u", esp_err_to_name(rc),
             out ? out->http_status : -1, (unsigned)clen);
    return rc;
}

#else /* !CONFIG_LTE_ENABLE */

/** @brief Stub: returns ESP_ERR_NOT_SUPPORTED when CONFIG_LTE_ENABLE is unset. */
esp_err_t lte_start(void)
{
    ESP_LOGI(TAG, "LTE disabled (CONFIG_LTE_ENABLE=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

/** @brief Stub: zero status with last_error noting LTE disabled. */
esp_err_t lte_get_status(lte_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->last_error, "CONFIG_LTE_ENABLE=n", sizeof(out->last_error) - 1);
    return ESP_OK;
}

/** @brief Stub: ESP_ERR_NOT_SUPPORTED. */
esp_err_t lte_refresh(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/** @brief Stub: ESP_ERR_NOT_SUPPORTED. */
esp_err_t lte_selftest(char *report, size_t len)
{
    if (report && len) {
        snprintf(report, len, "LTE disabled (CONFIG_LTE_ENABLE=n)");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/** @brief Stub: ESP_ERR_NOT_SUPPORTED. */
esp_err_t lte_reconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/** @brief Stub: empty GPS (gps_ok false). */
esp_err_t lte_gps_get(lte_gps_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

/** @brief Stub: no wall-clock sync. */
uint64_t lte_time_now_ms(void)
{
    return 0;
}

esp_err_t lte_time_get(lte_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

esp_err_t lte_http_post(const char *url, const char *body, lte_http_result_t *out)
{
    return lte_http_post_recv(url, body, NULL, NULL, 0, NULL, out);
}

esp_err_t lte_http_post_recv(const char *url, const char *body,
                                 const lte_http_req_headers_t *hdr,
                                 char *resp_buf, size_t resp_buf_len, size_t *resp_len,
                                 lte_http_result_t *out)
{
    (void)url;
    (void)body;
    (void)hdr;
    (void)resp_buf;
    (void)resp_buf_len;
    if (resp_len) {
        *resp_len = 0;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->error, sizeof(out->error), "CONFIG_LTE_ENABLE=n");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lte_http_get(const char *url, char *buf, size_t buf_len, size_t *out_len,
                          lte_http_result_t *out)
{
    (void)url;
    (void)buf;
    (void)buf_len;
    if (out_len) {
        *out_len = 0;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->error, sizeof(out->error), "CONFIG_LTE_ENABLE=n");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lte_http_get_stream(const char *url, lte_http_chunk_cb_t cb, void *ctx,
                                  size_t *content_length_out, lte_http_result_t *out)
{
    (void)url;
    (void)cb;
    (void)ctx;
    if (content_length_out) {
        *content_length_out = 0;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->error, sizeof(out->error), "CONFIG_LTE_ENABLE=n");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void lte_suspend_bg_at(bool suspend)
{
    (void)suspend;
}

#endif

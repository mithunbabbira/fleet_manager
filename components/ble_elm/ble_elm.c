#include "ble_elm.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "profile_store.h"
#include "telemetry_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_elm";

#define BLE_ELM_MAX_DEVICES 16
#define BLE_ELM_RX_STREAM_SIZE 512
#define BLE_ELM_RECONNECT_MIN_MS 1000
#define BLE_ELM_RECONNECT_MAX_MS 30000

/* Default Nordic UART Service (NUS) UUIDs. NimBLE stores UUID128 bytes in
 * little-endian order, i.e. reversed from the canonical dashed string form:
 *   6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * -> 9e ca dc 24 0e e5 a9 e0 93 f3 a3 b5 01 00 40 6e
 */
static ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static bool s_initialized;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_scan_done_sem;
static StreamBufferHandle_t s_rx_stream;

/* Scan state */
static ble_elm_device_t s_scan_results[BLE_ELM_MAX_DEVICES];
static int s_scan_count;
static bool s_scanning;

/* Connection / GATT discovery state */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_connected;
static uint16_t s_svc_start_handle;
static uint16_t s_svc_end_handle;
static uint16_t s_rx_val_handle;
static uint16_t s_tx_val_handle;
static uint16_t s_tx_cccd_handle;
static bool s_rx_write_no_rsp;
static bool s_chars_discovered;
static bool s_notify_subscribed;

/* Last/bonded peer for reconnect */
static uint8_t s_last_addr[6];
static uint8_t s_last_addr_type;
static bool s_have_last_addr;

/* Auto-reconnect */
static volatile bool s_auto_reconnect_enabled;
static TaskHandle_t s_reconnect_task;

static int ble_elm_gap_event(struct ble_gap_event *event, void *arg);

/* ---------- helpers ---------- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parses a dashed UUID128 string ("6E400001-B5A3-...") into a NimBLE
 * little-endian ble_uuid128_t (reversed byte order relative to the string). */
static bool parse_uuid128_str(const char *s, ble_uuid128_t *out)
{
    if (!s || !out) return false;
    uint8_t raw[16];
    int n = 0;
    for (const char *p = s; *p; ) {
        if (*p == '-') { p++; continue; }
        int hi = hex_val(*p++);
        if (hi < 0 || *p == '\0') return false;
        int lo = hex_val(*p++);
        if (lo < 0) return false;
        if (n >= 16) return false;
        raw[n++] = (uint8_t)((hi << 4) | lo);
    }
    if (n != 16) return false;
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; ++i) out->value[i] = raw[15 - i];
    return true;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void publish_elm_event(telemetry_elm_event_kind_t kind)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_ELM_EVENT;
    msg.elm_event.kind = kind;
    msg.elm_event.ts_ms = now_ms();
    telemetry_publish(&msg);
}

static void publish_error(telemetry_error_code_t code, const char *text)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_ERROR;
    msg.error.code = code;
    if (text) {
        snprintf(msg.error.message, sizeof(msg.error.message), "%s", text);
    }
    msg.error.ts_ms = now_ms();
    telemetry_publish(&msg);
}

static void load_config_overrides(void)
{
    ble_bond_t bond;
    if (profile_store_get_bond(&bond) != ESP_OK) {
        return;
    }
    if (bond.service_uuid[0] && bond.rx_uuid[0] && bond.tx_uuid[0]) {
        ble_uuid128_t svc, rx, tx;
        if (parse_uuid128_str(bond.service_uuid, &svc) &&
            parse_uuid128_str(bond.rx_uuid, &rx) &&
            parse_uuid128_str(bond.tx_uuid, &tx)) {
            s_svc_uuid = svc;
            s_rx_uuid = rx;
            s_tx_uuid = tx;
            ESP_LOGI(TAG, "using UUID overrides from profile_store bond");
        } else {
            ESP_LOGW(TAG, "bond UUID strings malformed; keeping NUS defaults");
        }
    }
    if (bond.addr_set) {
        memcpy(s_last_addr, bond.addr, 6);
        s_last_addr_type = BLE_ADDR_PUBLIC;
        s_have_last_addr = true;
    }
}

static void persist_bond_addr(const uint8_t addr[6], const char *name)
{
    ble_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    if (profile_store_get_bond(&bond) != ESP_OK) {
        memset(&bond, 0, sizeof(bond));
    }
    memcpy(bond.addr, addr, 6);
    bond.addr_set = true;
    if (name && name[0]) {
        snprintf(bond.name, sizeof(bond.name), "%s", name);
    }
    profile_store_set_bond(&bond);
}

static void wake_reconnect_task(void)
{
    if (s_reconnect_task) {
        xTaskNotifyGive(s_reconnect_task);
    }
}

/* ---------- scan result bookkeeping ---------- */

static void scan_results_clear(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(s_scan_results, 0, sizeof(s_scan_results));
    s_scan_count = 0;
    xSemaphoreGive(s_state_mutex);
}

static void scan_results_add(const uint8_t addr[6], uint8_t addr_type,
                              int8_t rssi, const uint8_t *name, uint8_t name_len)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_scan_count; ++i) {
        if (memcmp(s_scan_results[i].addr, addr, 6) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (s_scan_count >= BLE_ELM_MAX_DEVICES) {
            xSemaphoreGive(s_state_mutex);
            return;
        }
        idx = s_scan_count++;
        memset(&s_scan_results[idx], 0, sizeof(s_scan_results[idx]));
        memcpy(s_scan_results[idx].addr, addr, 6);
        s_scan_results[idx].addr_type = addr_type;
        s_scan_results[idx].rssi = rssi;
    }

    if (rssi > s_scan_results[idx].rssi) {
        s_scan_results[idx].rssi = rssi;
    }
    if (s_scan_results[idx].name[0] == '\0' && name && name_len > 0) {
        size_t cpy = name_len < sizeof(s_scan_results[idx].name) - 1
                         ? name_len
                         : sizeof(s_scan_results[idx].name) - 1;
        memcpy(s_scan_results[idx].name, name, cpy);
        s_scan_results[idx].name[cpy] = '\0';
    }
    xSemaphoreGive(s_state_mutex);
}

static uint8_t scan_results_find_addr_type(const uint8_t addr[6])
{
    uint8_t type = BLE_ADDR_PUBLIC;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (int i = 0; i < s_scan_count; ++i) {
        if (memcmp(s_scan_results[i].addr, addr, 6) == 0) {
            type = s_scan_results[i].addr_type;
            break;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return type;
}

/* ---------- GATT discovery chain ---------- */

static void reset_gatt_state(void)
{
    s_svc_start_handle = 0;
    s_svc_end_handle = 0;
    s_rx_val_handle = 0;
    s_tx_val_handle = 0;
    s_tx_cccd_handle = 0;
    s_rx_write_no_rsp = false;
    s_chars_discovered = false;
    s_notify_subscribed = false;
}

static void maybe_mark_ready(void)
{
    if (s_chars_discovered && s_notify_subscribed) {
        ESP_LOGI(TAG, "NUS transport ready (conn_handle=%d)", s_conn_handle);
        publish_elm_event(TELEMETRY_ELM_EVENT_CONNECTED);
        persist_bond_addr(s_last_addr, NULL);
    }
}

static int subscribe_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (conn_handle != s_conn_handle) {
        return 0;
    }
    if (error->status == 0) {
        s_notify_subscribed = true;
        maybe_mark_ready();
    } else {
        ESP_LOGE(TAG, "CCCD subscribe write failed; status=%d", error->status);
    }
    return 0;
}

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    if (conn_handle != s_conn_handle) {
        return 0;
    }
    if (error->status == 0 && dsc != NULL) {
        if (chr_val_handle == s_tx_val_handle &&
            ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_tx_cccd_handle = dsc->handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_tx_cccd_handle == 0) {
            ESP_LOGE(TAG, "TX characteristic has no CCCD; cannot subscribe");
            publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "no CCCD for TX char");
            return 0;
        }
        uint8_t value[2] = { 0x01, 0x00 };
        int rc = ble_gattc_write_flat(s_conn_handle, s_tx_cccd_handle, value, sizeof(value),
                                       subscribe_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to write CCCD; rc=%d", rc);
            publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "CCCD write failed");
        }
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (conn_handle != s_conn_handle) {
        return 0;
    }
    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &s_rx_uuid.u) == 0) {
            s_rx_val_handle = chr->val_handle;
            s_rx_write_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
        } else if (ble_uuid_cmp(&chr->uuid.u, &s_tx_uuid.u) == 0) {
            s_tx_val_handle = chr->val_handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_rx_val_handle == 0 || s_tx_val_handle == 0) {
            ESP_LOGE(TAG, "NUS RX/TX characteristic missing (rx=%d tx=%d)",
                     s_rx_val_handle, s_tx_val_handle);
            publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "NUS RX/TX char missing");
            return 0;
        }
        s_chars_discovered = true;
        int rc = ble_gattc_disc_all_dscs(s_conn_handle, s_tx_val_handle, s_svc_end_handle,
                                          dsc_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to start descriptor discovery; rc=%d", rc);
            publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "descriptor discovery failed");
        }
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (conn_handle != s_conn_handle) {
        return 0;
    }
    if (error->status == 0 && service != NULL) {
        s_svc_start_handle = service->start_handle;
        s_svc_end_handle = service->end_handle;
        int rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start_handle, s_svc_end_handle,
                                          chr_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to start characteristic discovery; rc=%d", rc);
            publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "char discovery failed");
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s_svc_start_handle == 0) {
        ESP_LOGE(TAG, "NUS service not found on peer");
        publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "NUS service not found");
    }
    return 0;
}

static void start_gatt_discovery(void)
{
    reset_gatt_state();
    int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_svc_uuid.u, svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start service discovery; rc=%d", rc);
        publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "service discovery failed");
    }
}

/* ---------- GAP event handling ---------- */

static void handle_disc_event(struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        scan_results_add(disc->addr.val, disc->addr.type, disc->rssi, NULL, 0);
        return;
    }
    scan_results_add(disc->addr.val, disc->addr.type, disc->rssi, fields.name, fields.name_len);
}

static void on_gap_connected(uint16_t conn_handle, int status)
{
    if (status != 0) {
        ESP_LOGW(TAG, "connect attempt failed; status=%d", status);
        publish_error(TELEMETRY_ERROR_BLE_CONNECT_FAIL, "connect attempt failed");
        if (s_auto_reconnect_enabled && s_have_last_addr) {
            wake_reconnect_task();
        }
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_conn_handle = conn_handle;
    s_connected = true;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "BLE connected; conn_handle=%d", conn_handle);
    start_gatt_discovery();
}

static void on_gap_disconnected(int reason)
{
    ESP_LOGI(TAG, "BLE disconnected; reason=%d", reason);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    reset_gatt_state();
    xSemaphoreGive(s_state_mutex);

    publish_elm_event(TELEMETRY_ELM_EVENT_DISCONNECTED);

    if (s_auto_reconnect_enabled && s_have_last_addr) {
        wake_reconnect_task();
    }
}

static void handle_notify_rx(struct ble_gap_event *event)
{
    if (event->notify_rx.attr_handle != s_tx_val_handle || !event->notify_rx.om) {
        return;
    }
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    uint8_t tmp[256];
    if (len > sizeof(tmp)) {
        len = sizeof(tmp);
    }
    if (os_mbuf_copydata(event->notify_rx.om, 0, len, tmp) == 0 && s_rx_stream) {
        xStreamBufferSend(s_rx_stream, tmp, len, 0);
    }
}

static int ble_elm_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_disc_event(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        xSemaphoreGive(s_scan_done_sem);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        on_gap_connected(event->connect.conn_handle, event->connect.status);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        on_gap_disconnected(event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_notify_rx(event);
        return 0;

    default:
        return 0;
    }
}

/* ---------- NimBLE host lifecycle ---------- */

static void ble_elm_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void ble_elm_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed; rc=%d", rc);
    }
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void ble_elm_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- elm_transport_t implementation ---------- */

static esp_err_t ble_elm_transport_write(struct elm_transport *t, const uint8_t *data, size_t len)
{
    (void)t;
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected || s_rx_val_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int rc;
    if (s_rx_write_no_rsp) {
        rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_rx_val_handle, data, (uint16_t)len);
    } else {
        rc = ble_gattc_write_flat(s_conn_handle, s_rx_val_handle, data, (uint16_t)len, NULL, NULL);
    }
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t ble_elm_transport_read_line(struct elm_transport *t, char *buf, size_t buflen,
                                              uint32_t timeout_ms)
{
    (void)t;
    if (!buf || buflen == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t n = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t remaining = (now >= deadline) ? 0 : (deadline - now);

        uint8_t byte;
        size_t got = xStreamBufferReceive(s_rx_stream, &byte, 1, remaining);
        if (got == 0) {
            break; /* idle/overall timeout */
        }
        if (byte == '>') {
            break;
        }
        if (n + 1 < buflen) {
            buf[n++] = (char)byte;
        }
    }
    buf[n] = '\0';
    return (n > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool ble_elm_transport_is_ready(struct elm_transport *t)
{
    (void)t;
    return s_connected && s_chars_discovered && s_notify_subscribed;
}

static elm_transport_t s_transport = {
    .ctx = NULL,
    .write = ble_elm_transport_write,
    .read_line = ble_elm_transport_read_line,
    .is_ready = ble_elm_transport_is_ready,
};

/* ---------- auto-reconnect task ---------- */

static void reconnect_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t backoff_ms = BLE_ELM_RECONNECT_MIN_MS;
        while (s_auto_reconnect_enabled && !s_connected && s_have_last_addr) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (!s_auto_reconnect_enabled || s_connected) {
                break;
            }
            ESP_LOGI(TAG, "auto-reconnect attempt (backoff=%lu ms)", (unsigned long)backoff_ms);
            ble_elm_connect_addr(s_last_addr);

            /* Give the stack a moment to report the connect result before
             * deciding whether another attempt is needed. */
            vTaskDelay(pdMS_TO_TICKS(500));

            backoff_ms *= 2;
            if (backoff_ms > BLE_ELM_RECONNECT_MAX_MS) {
                backoff_ms = BLE_ELM_RECONNECT_MAX_MS;
            }
        }
    }
}

/* ---------- public API ---------- */

esp_err_t ble_elm_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_scan_done_sem = xSemaphoreCreateBinary();
    s_rx_stream = xStreamBufferCreate(BLE_ELM_RX_STREAM_SIZE, 1);
    if (!s_state_mutex || !s_scan_done_sem || !s_rx_stream) {
        return ESP_ERR_NO_MEM;
    }

    load_config_overrides();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed; err=%d", err);
        publish_elm_event(TELEMETRY_ELM_EVENT_INIT_FAIL);
        return err;
    }

    ble_hs_cfg.reset_cb = ble_elm_on_reset;
    ble_hs_cfg.sync_cb = ble_elm_on_sync;

    nimble_port_freertos_init(ble_elm_host_task);

    s_initialized = true;
    publish_elm_event(TELEMETRY_ELM_EVENT_INIT_OK);
    ESP_LOGI(TAG, "ble_elm initialized");
    return ESP_OK;
}

esp_err_t ble_elm_start_scan(uint32_t duration_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0) {
        duration_ms = CONFIG_ELM_BLE_SCAN_MS;
    }

    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }

    scan_results_clear();
    xSemaphoreTake(s_scan_done_sem, 0); /* drain any stale signal */

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.passive = 1;
    disc_params.filter_duplicates = 0;

    s_scanning = true;
    rc = ble_gap_disc(own_addr_type, (int32_t)duration_ms, &disc_params, ble_elm_gap_event, NULL);
    if (rc != 0) {
        s_scanning = false;
        ESP_LOGE(TAG, "ble_gap_disc failed; rc=%d", rc);
        return ESP_FAIL;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(duration_ms) + pdMS_TO_TICKS(1000);
    xSemaphoreTake(s_scan_done_sem, wait_ticks);
    s_scanning = false;
    return ESP_OK;
}

esp_err_t ble_elm_get_scan_results(ble_elm_device_t *out, int max, int *count)
{
    if (!out || max <= 0 || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_results, (size_t)n * sizeof(ble_elm_device_t));
    xSemaphoreGive(s_state_mutex);
    *count = n;
    return ESP_OK;
}

esp_err_t ble_elm_connect_addr(const uint8_t addr[6])
{
    if (!s_initialized || !addr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
        return ESP_FAIL;
    }

    ble_addr_t peer_addr;
    peer_addr.type = scan_results_find_addr_type(addr);
    memcpy(peer_addr.val, addr, 6);

    memcpy(s_last_addr, addr, 6);
    s_last_addr_type = peer_addr.type;
    s_have_last_addr = true;

    rc = ble_gap_connect(own_addr_type, &peer_addr, 30000, NULL, ble_elm_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed; rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_elm_disconnect(void)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

bool ble_elm_is_connected(void)
{
    return s_connected;
}

elm_transport_t *ble_elm_get_transport(void)
{
    return &s_transport;
}

esp_err_t ble_elm_start_auto_reconnect(void)
{
    if (!s_reconnect_task) {
        BaseType_t ok = xTaskCreate(reconnect_task_fn, "ble_reconnect", 4096, NULL, 5,
                                     &s_reconnect_task);
        if (ok != pdPASS) {
            s_reconnect_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_auto_reconnect_enabled = true;
    if (!s_connected && s_have_last_addr) {
        wake_reconnect_task();
    }
    return ESP_OK;
}

esp_err_t ble_elm_stop_auto_reconnect(void)
{
    s_auto_reconnect_enabled = false;
    return ESP_OK;
}

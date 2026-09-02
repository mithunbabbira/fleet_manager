#include "can_obd.h"
#include "mcp2515.h"
#include "obd_isotp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "can_obd";

#define SPI_CLOCK_HZ        (1 * 1000 * 1000) /* conservative through TXS0108E */
#define LINK_TASK_STACK     4096
#define LINK_TASK_PRIO      5
#define PROBE_TIMEOUT_MS    800
#define PROBE_BACKOFF_MIN_S 2
#define PROBE_BACKOFF_MAX_S 30
#define LINK_FAIL_THRESHOLD 3

#define NVS_NS       "elm"
#define NVS_KEY_PROT "can_proto"

typedef struct {
    const char *name;      /* protocol string for UI / uplink */
    mcp_bitrate_t bitrate;
    bool ext;              /* 29-bit addressing */
    uint32_t req_id;       /* functional request id */
    uint32_t resp_filter;  /* RX filter id */
    uint32_t resp_mask;    /* RX filter mask */
} obd_protocol_t;

static const obd_protocol_t k_protocols[] = {
    {"ISO15765-4 CAN11/500", MCP_BITRATE_500K, false, 0x7DF, 0x7E8, 0x7F8},
    {"ISO15765-4 CAN11/250", MCP_BITRATE_250K, false, 0x7DF, 0x7E8, 0x7F8},
    {"ISO15765-4 CAN29/500", MCP_BITRATE_500K, true, 0x18DB33F1, 0x18DAF100, 0x1FFFFF00},
    {"ISO15765-4 CAN29/250", MCP_BITRATE_250K, true, 0x18DB33F1, 0x18DAF100, 0x1FFFFF00},
};
#define PROTOCOL_COUNT ((int)(sizeof(k_protocols) / sizeof(k_protocols[0])))

static SemaphoreHandle_t s_bus_mutex;
static volatile bool s_link_ready;
static volatile int s_active_proto = -1;
static volatile int s_consec_fail;
static bool s_started;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

/** @brief Map response ID → physical TX ID for ISO-TP FC. */
static uint32_t fc_dest_id(const obd_protocol_t *p, uint32_t resp_id)
{
    if (p->ext) {
        /* 0x18DAF1xx (ECU->tester)  ->  0x18DAxxF1 (tester->ECU) */
        return 0x18DA00F1u | ((resp_id & 0xFFu) << 8);
    }
    /* 0x7E8..0x7EF -> 0x7E0..0x7E7 */
    return resp_id - 8;
}

/** @brief Soft accept: id matches protocol filter/mask. */
static bool resp_id_matches(const obd_protocol_t *p, uint32_t id)
{
    return (id & p->resp_mask) == (p->resp_filter & p->resp_mask);
}

/**
 * @brief Locked SF TX + SF/FF/CF RX; send FC on NEED_FC.
 * @note Offline: abort after 250 ms, log TEC/REC/EFLG, ESP_FAIL.
 */
static esp_err_t transact_locked(const obd_protocol_t *p, const char *cmd,
                                 char *resp, size_t resp_len, uint32_t timeout_ms)
{
    mcp_can_frame_t tx = {.id = p->req_id, .ext = p->ext, .dlc = 8};
    if (obd_isotp_build_sf(cmd, tx.data) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Drain frames from a previous (timed-out) exchange. */
    mcp_can_frame_t rx;
    while (mcp2515_receive(&rx)) {
    }

    esp_err_t err = mcp2515_send(&tx);
    if (err != ESP_OK) {
        return err;
    }

    uint64_t start = now_ms();
    uint64_t deadline = start + timeout_ms;
    bool tx_acked = false;
    obd_isotp_rx_t asm_rx;
    obd_isotp_rx_reset(&asm_rx);

    while (now_ms() < deadline) {
        if (!tx_acked) {
            if (mcp2515_tx_done()) {
                tx_acked = true;
            } else if (now_ms() - start > 250) {
                break; /* no ACK in 250 ms: dead bus, don't burn the timeout */
            }
        }

        if (mcp2515_receive(&rx)) {
            if (!resp_id_matches(p, rx.id)) {
                continue;
            }
            obd_isotp_rx_status_t st =
                obd_isotp_rx_feed(&asm_rx, rx.id, rx.data, rx.dlc);
            if (st == OBD_ISOTP_RX_NEED_FC) {
                mcp_can_frame_t fc = {
                    .id = fc_dest_id(p, rx.id), .ext = p->ext, .dlc = 8};
                obd_isotp_build_fc(fc.data);
                mcp2515_send(&fc);
            } else if (st == OBD_ISOTP_RX_COMPLETE) {
                if (obd_isotp_payload_hex(&asm_rx, resp, resp_len) < 0) {
                    return ESP_ERR_NO_MEM;
                }
                return ESP_OK;
            } else if (st == OBD_ISOTP_RX_ERROR) {
                obd_isotp_rx_reset(&asm_rx);
            }
            continue; /* keep draining without sleeping */
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (!tx_acked) {
        mcp2515_tx_abort();
        uint8_t tec, rec, eflg;
        mcp2515_read_errors(&tec, &rec, &eflg);
        ESP_LOGW(TAG, "TX not acked (%s): TEC=%u REC=%u EFLG=0x%02X", cmd, tec,
                 rec, eflg);
        return ESP_FAIL; /* nobody on the bus at this bitrate */
    }
    return ESP_ERR_NOT_FOUND; /* bus alive but no ECU answered: "NO DATA" */
}

static esp_err_t apply_protocol(int idx)
{
    const obd_protocol_t *p = &k_protocols[idx];
    return mcp2515_configure(p->bitrate, p->ext, p->resp_filter, p->resp_mask);
}

static void save_protocol_nvs(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_PROT, (uint8_t)idx);
    nvs_commit(h);
    nvs_close(h);
}

static int load_protocol_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_PROT, &v);
    nvs_close(h);
    if (err != ESP_OK || v >= PROTOCOL_COUNT) {
        return -1;
    }
    return (int)v;
}

/** @brief Kconfig pin index or -1 for auto. */
static int pinned_protocol(void)
{
#if CONFIG_CAN_OBD_PROTO_11_500
    return 0;
#elif CONFIG_CAN_OBD_PROTO_11_250
    return 1;
#elif CONFIG_CAN_OBD_PROTO_29_500
    return 2;
#elif CONFIG_CAN_OBD_PROTO_29_250
    return 3;
#else
    return -1;
#endif
}

/** @brief Configure candidate; probe 0100 expect 4100…. */
static bool probe_candidate(int idx)
{
    if (apply_protocol(idx) != ESP_OK) {
        return false;
    }
    char resp[64];
    esp_err_t err = transact_locked(&k_protocols[idx], "0100", resp,
                                    sizeof(resp), PROBE_TIMEOUT_MS);
    if (err == ESP_OK && strncmp(resp, "4100", 4) == 0) {
        ESP_LOGI(TAG, "protocol %s: ECU answered 0100 -> %s",
                 k_protocols[idx].name, resp);
        return true;
    }
    ESP_LOGD(TAG, "protocol %s: no answer (%s)", k_protocols[idx].name,
             esp_err_to_name(err));
    return false;
}

/** @brief Pin → NVS-first → full sweep. */
static int detect_protocol(void)
{
    int pinned = pinned_protocol();
    if (pinned >= 0) {
        return probe_candidate(pinned) ? pinned : -1;
    }

    int saved = load_protocol_nvs();
    if (saved >= 0 && probe_candidate(saved)) {
        return saved;
    }
    for (int i = 0; i < PROTOCOL_COUNT; i++) {
        if (i == saved) {
            continue;
        }
        if (probe_candidate(i)) {
            return i;
        }
    }
    return -1;
}

/** @brief Detect when down; backoff; re-detect after consecutive TX fails. */
static void link_task(void *arg)
{
    (void)arg;
    uint32_t backoff_s = PROBE_BACKOFF_MIN_S;

    for (;;) {
        if (!s_link_ready) {
            int found = -1;
            if (xSemaphoreTake(s_bus_mutex, portMAX_DELAY) == pdTRUE) {
                found = detect_protocol();
                xSemaphoreGive(s_bus_mutex);
            }
            if (found >= 0) {
                s_active_proto = found;
                s_consec_fail = 0;
                s_link_ready = true;
                backoff_s = PROBE_BACKOFF_MIN_S;
                save_protocol_nvs(found);
                ESP_LOGI(TAG, "link up: %s", k_protocols[found].name);
            } else {
                ESP_LOGW(TAG, "no ECU found on any protocol; retry in %lus",
                         (unsigned long)backoff_s);
                vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
                backoff_s *= 2;
                if (backoff_s > PROBE_BACKOFF_MAX_S) {
                    backoff_s = PROBE_BACKOFF_MAX_S;
                }
            }
            continue;
        }

        if (s_consec_fail >= LINK_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "link lost (%d consecutive failures); reprobing",
                     s_consec_fail);
            s_link_ready = false;
            s_active_proto = -1;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t can_obd_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_bus_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return mcp2515_init(CONFIG_CAN_OBD_SPI_SCK_GPIO, CONFIG_CAN_OBD_SPI_MOSI_GPIO,
                        CONFIG_CAN_OBD_SPI_MISO_GPIO, CONFIG_CAN_OBD_SPI_CS_GPIO,
                        SPI_CLOCK_HZ);
}

esp_err_t can_obd_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (xTaskCreate(link_task, "can_link", LINK_TASK_STACK, NULL,
                    LINK_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool can_obd_is_ready(void)
{
    return s_link_ready;
}

esp_err_t can_obd_transact(const char *cmd, char *resp, size_t resp_len,
                           uint32_t timeout_ms)
{
    if (cmd == NULL || resp == NULL || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    resp[0] = '\0';

    if ((cmd[0] == 'A' || cmd[0] == 'a') && (cmd[1] == 'T' || cmd[1] == 't')) {
        return ESP_ERR_NOT_SUPPORTED; /* no ELM chip: AT commands are gone */
    }
    if (!s_link_ready || s_active_proto < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = 1000;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int proto = s_active_proto;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (proto >= 0) {
        err = transact_locked(&k_protocols[proto], cmd, resp, resp_len,
                              timeout_ms);
    }
    xSemaphoreGive(s_bus_mutex);

    if (err == ESP_OK) {
        s_consec_fail = 0;
    } else if (err == ESP_FAIL || err == ESP_ERR_TIMEOUT) {
        s_consec_fail++;
    }
    return err;
}

void can_obd_get_protocol(char *out, size_t out_len)
{
    int proto = s_active_proto;
    if (proto >= 0 && proto < PROTOCOL_COUNT) {
        snprintf(out, out_len, "%s", k_protocols[proto].name);
    } else {
        snprintf(out, out_len, "%s", "none");
    }
}

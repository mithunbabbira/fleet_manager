#include "obd_poller.h"

#include "can_obd.h"
#include "obd_codec.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "obd_poller";

#define POLLER_TASK_STACK 4096
#define POLLER_TASK_PRIO  5
#define CMD_TIMEOUT_MS    1000
#define FRESH_MS          15000u

typedef struct {
    const char *cmd;
    const char *decode_key;
    uint32_t interval_ms;
} poll_item_t;

static const poll_item_t k_items[] = {
    {"010C", "rpm", 500},
    {"010D", "speed", 500},
    {"0105", "coolant", 2000},
    {"0111", "throttle", 1000},
};
#define ITEM_COUNT ((int)(sizeof(k_items) / sizeof(k_items[0])))

typedef struct {
    bool have;
    bool ok;
    double value;
    char raw[48];
    uint64_t ts_ms;
} pid_sample_t;

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static bool s_enabled = true;
static uint64_t s_cmds_ok;
static uint64_t s_cmds_fail;
static uint64_t s_boot_ms;
static pid_sample_t s_rpm, s_speed, s_coolant, s_throttle;
static uint64_t s_last_fire_ms[ITEM_COUNT];

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static pid_sample_t *sample_for_key(const char *key)
{
    if (strcmp(key, "rpm") == 0) {
        return &s_rpm;
    }
    if (strcmp(key, "speed") == 0) {
        return &s_speed;
    }
    if (strcmp(key, "coolant") == 0) {
        return &s_coolant;
    }
    if (strcmp(key, "throttle") == 0) {
        return &s_throttle;
    }
    return NULL;
}

static void store_sample(const char *decode_key, const char *resp, bool decode_ok)
{
    pid_sample_t *s = sample_for_key(decode_key);
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->have = true;
    s->ts_ms = now_ms();
    if (!decode_ok) {
        return;
    }
    obd_decoded_t decoded;
    if (obd_codec_decode_named(decode_key, resp, &decoded) && decoded.ok) {
        s->ok = true;
        s->value = decoded.value;
        snprintf(s->raw, sizeof(s->raw), "%s", decoded.raw_hex);
    }
}

static void fill_view(obd_pid_view_t *v, const pid_sample_t *s, uint64_t now)
{
    memset(v, 0, sizeof(*v));
    if (s == NULL || !s->have) {
        return;
    }
    v->valid = true;
    v->ok = s->ok;
    v->value = s->value;
    snprintf(v->raw, sizeof(v->raw), "%s", s->raw);
    if (now >= s->ts_ms) {
        v->age_ms = (uint32_t)(now - s->ts_ms);
    } else {
        v->age_ms = UINT32_MAX;
    }
}

static void poll_one(const poll_item_t *it)
{
    char resp[128];
    esp_err_t err = can_obd_transact(it->cmd, resp, sizeof(resp), CMD_TIMEOUT_MS);
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (err == ESP_OK) {
        s_cmds_ok++;
        store_sample(it->decode_key, resp, true);
    } else {
        s_cmds_fail++;
        store_sample(it->decode_key, "", false);
        ESP_LOGD(TAG, "%s fail: %s", it->cmd, esp_err_to_name(err));
    }
    xSemaphoreGive(s_mu);
}

static void poller_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_enabled || !can_obd_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        uint64_t now = now_ms();
        bool any = false;
        for (int i = 0; i < ITEM_COUNT; i++) {
            if (s_last_fire_ms[i] != 0 &&
                (now - s_last_fire_ms[i]) < k_items[i].interval_ms) {
                continue;
            }
            s_last_fire_ms[i] = now;
            poll_one(&k_items[i]);
            any = true;
            now = now_ms();
        }
        vTaskDelay(pdMS_TO_TICKS(any ? 20 : 100));
    }
}

esp_err_t obd_poller_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
        if (s_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_boot_ms = now_ms();
    s_enabled = true;
    ESP_LOGI(TAG, "init (010C/010D/0105/0111)");
    return ESP_OK;
}

esp_err_t obd_poller_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(poller_task, "obd_poller", POLLER_TASK_STACK, NULL, POLLER_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t obd_poller_get_snapshot(obd_poller_snapshot_t *out)
{
    if (out == NULL || s_mu == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint64_t now = now_ms();
    memset(out, 0, sizeof(*out));
    can_obd_get_protocol(out->protocol, sizeof(out->protocol));
    out->can_ready = can_obd_is_ready();
    out->enabled = s_enabled;
    out->cmds_ok = s_cmds_ok;
    out->cmds_fail = s_cmds_fail;
    out->uptime_seconds = (uint32_t)((now - s_boot_ms) / 1000ULL);
    fill_view(&out->rpm, &s_rpm, now);
    fill_view(&out->speed, &s_speed, now);
    fill_view(&out->coolant, &s_coolant, now);
    fill_view(&out->throttle, &s_throttle, now);
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

bool obd_poller_has_fresh_pid(const obd_poller_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    const obd_pid_view_t *pids[] = {&snap->rpm, &snap->speed, &snap->coolant, &snap->throttle};
    for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
        const obd_pid_view_t *p = pids[i];
        if (p->valid && p->ok && p->age_ms <= FRESH_MS) {
            return true;
        }
    }
    return false;
}

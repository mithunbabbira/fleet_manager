#include "transport_serial.h"

#include "can_obd.h"
#include "cmd_policy.h"
#include "fw_ota.h"
#include "fw_ota_lte.h"
#include "net_lte.h"
#include "obd_codec.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"
#include "telemetry_uplink.h"
#include "store_sd.h"
#include "host_registry.h"
#include "transport_zigbee.h"

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static const char *TAG = "transport_serial";

#define CONSOLE_TASK_STACK   8192
#define CONSOLE_TASK_PRIO    4
#define TELEMETRY_TASK_STACK 3072
#define TELEMETRY_TASK_PRIO  3

#define LINE_BUF_LEN     256
#define RESP_BUF_LEN     256
#define METRICS_BUF_LEN  768
#define MAX_PROFILE_NAMES 16

#define TELEMETRY_POLL_MS  500

static bool s_started;
static TaskHandle_t s_console_task_handle;

static QueueHandle_t s_telemetry_queue;
static TaskHandle_t s_telemetry_task_handle;
static volatile bool s_telemetry_stop_requested;

/* ---- small string helpers ------------------------------------------- */

/** @brief Strip leading/trailing whitespace in place. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        ++s;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }
    return s;
}

/** @brief Lowercase ASCII string in place. */
static void str_lower(char *s)
{
    for (; *s != '\0'; ++s) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/** @brief Split a line into verb and remaining args (mutates line). */
static void split_verb_args(char *line, char **verb, char **args)
{
    *verb = line;
    char *p = line;
    while (*p != '\0' && !isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p == '\0') {
        *args = p;
        return;
    }
    *p = '\0';
    ++p;
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    *args = p;
}

/* ---- commands --------------------------------------------------------- */

/** @brief Print USB console command help. */
static void cmd_help(void)
{
    printf(
        "Commands:\n"
        "  help\n"
        "  config\n"
        "  provision <device_id> <node_id>\n"
        "  status\n"
        "  cmd <OBD>\n"
        "  vin\n"
        "  dtc read|clear\n"
        "  profiles\n"
        "  profile <name>\n"
        "  telemetry on|off\n"
        "  unsafe on|off\n"
        "  metrics\n"
        "  lte [reconnect|test]\n"
        "  uplink [on|off|now|qtest|device_id|node_id|interval|url|schema]\n"
        "  ota [status|run|force|url|device_id]\n"
        "  fleet hosts\n"
        "  fleet ingest <hex>\n");
}

/** @brief Map LTE OTA phase enum to a short string. */
static const char *ota_lte_phase_str(fw_ota_lte_phase_t p)
{
    switch (p) {
    case FW_OTA_LTE_IDLE: return "idle";
    case FW_OTA_LTE_CHECKING: return "checking";
    case FW_OTA_LTE_DOWNLOADING: return "downloading";
    case FW_OTA_LTE_NO_UPDATE: return "no_update";
    case FW_OTA_LTE_FAILED: return "failed";
    case FW_OTA_LTE_REBOOTING: return "rebooting";
    default: return "unknown";
    }
}

/** @brief Print fw_ota + fw_ota_lte status and config. */
static void cmd_ota_status(void)
{
    fw_ota_status_t ost;
    fw_ota_lte_status_t lst;
    fw_ota_lte_config_t cfg;
    fw_ota_get_status(&ost);
    fw_ota_lte_get_status(&lst);
    fw_ota_lte_get_config(&cfg);
    printf("ota: fw=%s boot=%s next=%s state=%d pending_verify=%s busy=%s\n",
           ost.fw_version, ost.running_partition, ost.update_partition, (int)ost.state,
           ost.pending_verify ? "yes" : "no",
           (fw_ota_is_busy() || fw_ota_lte_is_busy()) ? "yes" : "no");
    printf("lte_ota: phase=%s current=%s latest=%s update_available=%s applied=%s http=%d dl=%u err=\"%s\"\n",
           ota_lte_phase_str(lst.phase), lst.current_version, lst.manifest_version,
           lst.update_available ? "yes" : "no", lst.applied_version,
           lst.http_status, (unsigned)lst.bytes_downloaded, lst.error);
    printf("cfg: force=%s channel=%s device_id=%s\n",
           cfg.force ? "on" : "off", cfg.channel, cfg.device_id);
    printf("url: %s\n", cfg.manifest_url);
}

/** @brief Machine-readable config dump for PC Carrier Console. */
static void cmd_config(void)
{
    obd_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    profile_store_get_active(&profile);

    cmd_policy_config_t safety;
    memset(&safety, 0, sizeof(safety));
    profile_store_get_safety(&safety);

    telemetry_uplink_config_t uplink;
    memset(&uplink, 0, sizeof(uplink));
    telemetry_uplink_get_config(&uplink);

    fw_ota_lte_config_t ota;
    memset(&ota, 0, sizeof(ota));
    fw_ota_lte_get_config(&ota);

    printf("profile=%s\n", profile.name[0] ? profile.name : "(none)");
    printf("allow_unsafe=%s\n", safety.allow_unsafe ? "true" : "false");
    printf("uplink_enabled=%s\n", uplink.enabled ? "yes" : "no");
    printf("uplink_interval=%u\n", (unsigned)uplink.interval_s);
    printf("uplink_device_id=%s\n", uplink.device_id);
    printf("uplink_node_id=%s\n", uplink.node_id);
    printf("ota_force=%s\n", ota.force ? "on" : "off");
    printf("ota_device_id=%s\n", ota.device_id);
    printf("ota_url=%s\n", ota.manifest_url);

    telemetry_uplink_status_t ust;
    memset(&ust, 0, sizeof(ust));
    if (telemetry_uplink_get_status(&ust) == ESP_OK) {
        printf("uplink_url=%s\n", ust.url ? ust.url : "");
        printf("uplink_schema=%s\n", ust.schema_id ? ust.schema_id : "");
    }
    printf("provisioned=%s\n", telemetry_uplink_is_provisioned() ? "yes" : "no");
}

/** @brief One-shot device identity (device_id + node_id); syncs OTA device_id. */
static void cmd_provision(char *args)
{
    if (!args || !args[0]) {
        printf("usage: provision <device_id> <node_id>\n");
        return;
    }
    char *did = args;
    while (*did == ' ') {
        did++;
    }
    char *nid = did;
    while (*nid && !isspace((unsigned char)*nid)) {
        nid++;
    }
    if (*nid == '\0') {
        printf("usage: provision <device_id> <node_id>\n");
        return;
    }
    *nid++ = '\0';
    while (*nid == ' ') {
        nid++;
    }
    if (*nid == '\0') {
        printf("usage: provision <device_id> <node_id>\n");
        return;
    }
    esp_err_t err = telemetry_uplink_provision(did, nid);
    if (err != ESP_OK) {
        printf("provision: %s\n", esp_err_to_name(err));
        return;
    }
    printf("provision: ok device_id=%s node_id=%s (uplink still off — use 'uplink on')\n", did,
           nid);
}

/** @brief Mode 09 PID 02 VIN decode. */
static void cmd_vin(void)
{
    if (!can_obd_is_ready()) {
        printf("vin: CAN link not ready\n");
        return;
    }
    char resp[RESP_BUF_LEN];
    esp_err_t err = obd_poller_submit_raw("0902", resp, sizeof(resp), CONFIG_ELM_CMD_TIMEOUT_MS);
    if (err == ESP_ERR_NOT_ALLOWED) {
        printf("vin: blocked by safety policy\n");
        return;
    }
    if (err != ESP_OK) {
        printf("vin: failed (%s): %s\n", esp_err_to_name(err), resp);
        return;
    }
    char vin[18];
    if (obd_codec_parse_vin(resp, vin, sizeof(vin))) {
        printf("vin=%s\n", vin);
    } else {
        printf("vin: not reported (raw=%s)\n", resp);
    }
}

/** @brief Mode 03/07 DTC read or Mode 04 clear (gated by allow_unsafe). */
static void cmd_dtc(char *args)
{
    if (!can_obd_is_ready()) {
        printf("dtc: CAN link not ready\n");
        return;
    }
    if (!args || !args[0]) {
        printf("usage: dtc read|clear\n");
        return;
    }
    char *sub = NULL;
    char *rest = NULL;
    split_verb_args(args, &sub, &rest);
    (void)rest;
    str_lower(sub);

    if (strcmp(sub, "read") == 0) {
        const struct {
            const char *label;
            const char *cmd;
        } queries[] = {{"stored", "03"}, {"pending", "07"}};
        for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
            char resp[RESP_BUF_LEN];
            esp_err_t err = obd_poller_submit_raw(queries[i].cmd, resp, sizeof(resp),
                                                  CONFIG_ELM_CMD_TIMEOUT_MS);
            if (err == ESP_ERR_NOT_ALLOWED) {
                printf("dtc %s: blocked\n", queries[i].label);
                continue;
            }
            if (err != ESP_OK) {
                printf("dtc %s: failed (%s): %s\n", queries[i].label, esp_err_to_name(err), resp);
                continue;
            }
            char dtcs[8][6];
            int n = obd_codec_parse_dtcs(resp, dtcs, 8);
            printf("dtc_%s_count=%d\n", queries[i].label, n);
            for (int j = 0; j < n; ++j) {
                printf("dtc_%s_%d=%s\n", queries[i].label, j, dtcs[j]);
            }
        }
        return;
    }
    if (strcmp(sub, "clear") == 0) {
        char resp[RESP_BUF_LEN];
        esp_err_t err = obd_poller_submit_raw("04", resp, sizeof(resp), CONFIG_ELM_CMD_TIMEOUT_MS);
        if (err == ESP_ERR_NOT_ALLOWED) {
            printf("dtc clear: blocked — enable unsafe first\n");
            return;
        }
        if (err != ESP_OK) {
            printf("dtc clear: failed (%s): %s\n", esp_err_to_name(err), resp);
            return;
        }
        printf("dtc clear: ok raw=%s\n", resp);
        return;
    }
    printf("usage: dtc read|clear\n");
}

/**
 * @brief Handle `ota` (status|run|force|url|device_id) — LTE OTA only; no SoftAP bin upload.
 *
 * - status: fw_ota + fw_ota_lte phase/versions
 * - run: start background LTE OTA
 * - force on|off: persist NVS ota_force
 * - url <url>: persist firmware-check URL (NVS ota_manif), as typed
 */
static void cmd_ota(char *args)
{
    if (!args || !args[0]) {
        cmd_ota_status();
        return;
    }
    char *sub = NULL;
    char *rest = NULL;
    split_verb_args(args, &sub, &rest);
    str_lower(sub);

    if (strcmp(sub, "status") == 0 || strcmp(sub, "st") == 0) {
        cmd_ota_status();
        return;
    }
    if (strcmp(sub, "run") == 0) {
        esp_err_t err = fw_ota_lte_start_background();
        printf("ota run: %s\n", esp_err_to_name(err));
        return;
    }
    if (strcmp(sub, "force") == 0) {
        if (!rest || !rest[0]) {
            printf("usage: ota force on|off\n");
            return;
        }
        str_lower(rest);
        fw_ota_lte_config_t cfg;
        fw_ota_lte_get_config(&cfg);
        if (strcmp(rest, "on") == 0) {
            cfg.force = true;
        } else if (strcmp(rest, "off") == 0) {
            cfg.force = false;
        } else {
            printf("usage: ota force on|off\n");
            return;
        }
        printf("ota force: %s\n", esp_err_to_name(fw_ota_lte_set_config(&cfg)));
        return;
    }
    if (strcmp(sub, "url") == 0) {
        if (!rest || !rest[0]) {
            printf("usage: ota url <firmware-check-url>\n");
            return;
        }
        fw_ota_lte_config_t cfg;
        fw_ota_lte_get_config(&cfg);
        snprintf(cfg.manifest_url, sizeof(cfg.manifest_url), "%s", rest);
        printf("ota url: %s\n", esp_err_to_name(fw_ota_lte_set_config(&cfg)));
        return;
    }
    if (strcmp(sub, "device_id") == 0) {
        if (!rest || !rest[0]) {
            printf("usage: ota device_id <id>\n");
            return;
        }
        fw_ota_lte_config_t cfg;
        fw_ota_lte_get_config(&cfg);
        snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", rest);
        printf("ota device_id: %s\n", esp_err_to_name(fw_ota_lte_set_config(&cfg)));
        return;
    }
    printf("usage: ota [status|run|force on|off|url <url>|device_id <id>]\n");
}

/** @brief Handle `uplink` (status|on|off|now|qtest|device_id|node_id|interval). */
static void cmd_uplink(char *args)
{
    if (args && args[0]) {
        char *sub = NULL;
        char *rest = NULL;
        split_verb_args(args, &sub, &rest);
        str_lower(sub);

        if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
            if (strcmp(sub, "on") == 0 && !telemetry_uplink_is_provisioned()) {
                printf("uplink on: not provisioned — run 'provision <device_id> <node_id>' first\n");
                return;
            }
            telemetry_uplink_config_t cfg;
            if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
                printf("uplink: config unavailable\n");
                return;
            }
            cfg.enabled = (strcmp(sub, "on") == 0);
            esp_err_t err = telemetry_uplink_set_config(&cfg);
            printf("uplink %s: %s\n", sub, esp_err_to_name(err));
            return;
        }
        if (strcmp(sub, "now") == 0) {
            printf("uplink now: attempting POST (may take several seconds)...\n");
            esp_err_t err = telemetry_uplink_send_now();
            telemetry_uplink_status_t st;
            if (telemetry_uplink_get_status(&st) == ESP_OK) {
                printf("uplink now: %s http=%d reason=\"%s\" error=\"%s\"\n",
                       esp_err_to_name(err), st.last.http_status, st.last.reason,
                       st.last.error);
            } else {
                printf("uplink now: %s\n", esp_err_to_name(err));
            }
            return;
        }
        if (strcmp(sub, "qtest") == 0) {
            esp_err_t err = telemetry_uplink_queue_test_enqueue();
            telemetry_uplink_status_t st;
            telemetry_uplink_get_status(&st);
            store_sd_status_t sd;
            store_sd_get_status(&sd);
            printf("uplink qtest: %s sd=%s depth=%u drain_err=\"%s\" sd_err=\"%s\"\n",
                   esp_err_to_name(err), st.queue.sd_mounted ? "yes" : "no",
                   (unsigned)st.queue.queue_depth, st.queue.drain_error, sd.last_error);
            return;
        }
        if (strcmp(sub, "device_id") == 0) {
            if (!rest || !rest[0]) {
                printf("usage: uplink device_id <id>\n");
                return;
            }
            telemetry_uplink_config_t cfg;
            if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
                printf("uplink: config unavailable\n");
                return;
            }
            snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", rest);
            printf("uplink device_id: %s\n", esp_err_to_name(telemetry_uplink_set_config(&cfg)));
            return;
        }
        if (strcmp(sub, "node_id") == 0) {
            if (!rest || !rest[0]) {
                printf("usage: uplink node_id <id>\n");
                return;
            }
            telemetry_uplink_config_t cfg;
            if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
                printf("uplink: config unavailable\n");
                return;
            }
            snprintf(cfg.node_id, sizeof(cfg.node_id), "%s", rest);
            printf("uplink node_id: %s\n", esp_err_to_name(telemetry_uplink_set_config(&cfg)));
            return;
        }
        if (strcmp(sub, "interval") == 0) {
            if (!rest || !rest[0]) {
                printf("usage: uplink interval <seconds>\n");
                return;
            }
            unsigned v = (unsigned)strtoul(rest, NULL, 10);
            if (v < 5 || v > 3600) {
                printf("uplink interval: out of range (5–3600)\n");
                return;
            }
            telemetry_uplink_config_t cfg;
            if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
                printf("uplink: config unavailable\n");
                return;
            }
            cfg.interval_s = (uint16_t)v;
            printf("uplink interval: %s\n", esp_err_to_name(telemetry_uplink_set_config(&cfg)));
            return;
        }
        if (strcmp(sub, "url") == 0) {
            if (!rest || !rest[0]) {
                printf("usage: uplink url <telemetry-post-url>\n");
                return;
            }
            printf("uplink url: %s\n", esp_err_to_name(telemetry_uplink_set_post_url(rest)));
            return;
        }
        if (strcmp(sub, "schema") == 0) {
            if (!rest || !rest[0]) {
                printf("usage: uplink schema <schema-id>\n");
                return;
            }
            printf("uplink schema: %s\n", esp_err_to_name(telemetry_uplink_set_schema_id(rest)));
            return;
        }
    }

    telemetry_uplink_status_t st;
    if (telemetry_uplink_get_status(&st) != ESP_OK) {
        printf("uplink: status unavailable\n");
        return;
    }
    printf("uplink: enabled=%s interval=%us device_id=%s node_id=%s\n",
           st.config.enabled ? "yes" : "no", st.config.interval_s,
           st.config.device_id, st.config.node_id);
    printf("        last: ok=%s skipped=%s http=%d reason=\"%s\" error=\"%s\"\n",
           st.last.ok ? "yes" : "no", st.last.skipped ? "yes" : "no",
           st.last.http_status, st.last.reason, st.last.error);
    printf("        queue: sd=%s depth=%u bytes=%llu drain_err=\"%s\"\n",
           st.queue.sd_mounted ? "yes" : "no", (unsigned)st.queue.queue_depth,
           (unsigned long long)st.queue.queue_bytes, st.queue.drain_error);
    printf("        url=%s schemaId=%s\n", st.url, st.schema_id);
    net_lte_gps_t gps;
    if (net_lte_gps_get(&gps) == ESP_OK) {
        if (gps.gps_ok) {
            printf("        gps: ok lat=%.6f lng=%.6f age_ms=%u\n",
                   gps.lat, gps.lng, (unsigned)gps.age_ms);
        } else {
            printf("        gps: no fix\n");
        }
    }
}

/** @brief Handle `lte` (status|reconnect|test). */
static void cmd_lte(char *args)
{
    if (args && args[0]) {
        str_lower(args);
        if (strcmp(args, "reconnect") == 0) {
            esp_err_t err = net_lte_reconnect();
            printf("lte reconnect: %s\n", esp_err_to_name(err));
            return;
        }
        if (strcmp(args, "test") == 0) {
            printf("lte test: running modem internet self-test (may take up to ~90s)...\n");
            char report[768];
            esp_err_t err = net_lte_selftest(report, sizeof(report));
            printf("%s\n(result: %s)\n", report, esp_err_to_name(err));
            return;
        }
    }

    /* Live refresh so the printed values reflect the modem now, not boot. */
    net_lte_refresh();

    net_lte_status_t s;
    if (net_lte_get_status(&s) != ESP_OK) {
        printf("lte: status unavailable\n");
        return;
    }
    if (!s.enabled) {
        printf("lte: disabled (CONFIG_NET_LTE_ENABLE=n)\n");
        return;
    }
    printf("lte: uart_ok=%s sim=%s reg=%s attached=%s csq=%d(%ddBm) op=\"%s\" apn=\"%s\"\n",
           s.uart_ok ? "yes" : "no",
           s.sim_ready ? "yes" : "no",
           s.registered ? "yes" : "no",
           s.attached ? "yes" : "no",
           s.csq, s.rssi_dbm, s.operator_name, s.apn);
    printf("     module=\"%s\" note=\"%s\"\n", s.ati, s.last_error);
}

/** @brief Print CAN/poller/profile status plus metrics JSON. */
static void cmd_status(void)
{
    obd_profile_t profile;
    char protocol[32];
    memset(&profile, 0, sizeof(profile));
    profile_store_get_active(&profile);
    can_obd_get_protocol(protocol, sizeof(protocol));

    printf("can_ready=%s protocol=%s poller=%s profile=%s items=%d\n",
           can_obd_is_ready() ? "yes" : "no",
           protocol,
           obd_poller_is_enabled() ? "on" : "paused",
           profile.name[0] ? profile.name : "(none)",
           profile.item_count);

    char metrics[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(metrics, sizeof(metrics));
    printf("metrics=%s\n", metrics);
}

/** @brief Submit a raw OBD command via the poller. */
static void cmd_cmd(char *args)
{
    if (!args || !*args) {
        printf("usage: cmd <OBD>\n");
        return;
    }
    if (toupper((unsigned char)args[0]) == 'A' &&
        toupper((unsigned char)args[1]) == 'T') {
        printf("AT commands are not supported\n");
        return;
    }
    char resp[RESP_BUF_LEN];
    /* OBD protocol search can exceed the default; always allow full budget. */
    esp_err_t err = obd_poller_submit_raw(args, resp, sizeof(resp), CONFIG_ELM_CMD_TIMEOUT_MS);
    if (err == ESP_ERR_NOT_ALLOWED) {
        printf("BLOCKED by safety policy: %s\n", args);
        return;
    }
    if (err != ESP_OK) {
        printf("cmd failed (%s): %s\n", esp_err_to_name(err), resp);
        return;
    }
    printf("%s\n", resp);
}

/** @brief List stored OBD profiles and mark the active one. */
static void cmd_profiles(void)
{
    char names[MAX_PROFILE_NAMES][32];
    int count = 0;
    if (profile_store_list(names, MAX_PROFILE_NAMES, &count) != ESP_OK) {
        printf("list failed\n");
        return;
    }
    obd_profile_t active;
    memset(&active, 0, sizeof(active));
    profile_store_get_active(&active);
    printf("profiles (%d), active=%s\n", count, active.name);
    for (int i = 0; i < count && i < MAX_PROFILE_NAMES; ++i) {
        printf("  %s%s\n", names[i], strcmp(names[i], active.name) == 0 ? " *" : "");
    }
}

/** @brief Set active profile by name and reload the poller. */
static void cmd_profile(char *args)
{
    if (!args || !*args) {
        printf("usage: profile <name>\n");
        return;
    }
    esp_err_t err = profile_store_set_active(args);
    if (err != ESP_OK) {
        printf("set active failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = obd_poller_reload_active_profile();
    if (err != ESP_OK) {
        printf("reload failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("active profile -> %s\n", args);
}

/** @brief Task that prints subscribed telemetry samples to the console. */
static void telemetry_dump_task(void *arg)
{
    (void)arg;
    telemetry_msg_t msg;
    while (!s_telemetry_stop_requested) {
        if (xQueueReceive(s_telemetry_queue, &msg, pdMS_TO_TICKS(TELEMETRY_POLL_MS)) == pdTRUE) {
            if (msg.type == TELEMETRY_PID_SAMPLE) {
                printf("[telem] %s=%g %s ok=%d raw=%s\n",
                       msg.pid_sample.name, msg.pid_sample.value, msg.pid_sample.unit,
                       msg.pid_sample.ok, msg.pid_sample.raw_hex);
            } else if (msg.type == TELEMETRY_ELM_EVENT) {
                printf("[telem] elm_event=%d\n", (int)msg.elm_event.kind);
            } else if (msg.type == TELEMETRY_ERROR) {
                printf("[telem] error=%s cmd=%s\n", msg.error.message, msg.error.cmd);
            }
        }
    }
    s_telemetry_task_handle = NULL;
    vTaskDelete(NULL);
}

/** @brief Handle `telemetry on|off` dump subscription. */
static void cmd_telemetry(char *args)
{
    if (!args || !*args) {
        printf("usage: telemetry on|off\n");
        return;
    }
    str_lower(args);
    if (strcmp(args, "on") == 0) {
        if (s_telemetry_queue == NULL) {
            if (telemetry_subscribe(&s_telemetry_queue, 0xFFFFFFFFu) != ESP_OK) {
                printf("telemetry subscribe failed\n");
                return;
            }
        }
        s_telemetry_stop_requested = false;
        if (s_telemetry_task_handle == NULL) {
            xTaskCreate(telemetry_dump_task, "telem_dump", TELEMETRY_TASK_STACK, NULL,
                        TELEMETRY_TASK_PRIO, &s_telemetry_task_handle);
        }
        printf("telemetry dump ON\n");
    } else if (strcmp(args, "off") == 0) {
        s_telemetry_stop_requested = true;
        printf("telemetry dump OFF\n");
    } else {
        printf("usage: telemetry on|off\n");
    }
}

/** @brief Handle `unsafe on|off` (allow_unsafe / Mode 04 gate). */
static void cmd_unsafe(char *args)
{
    if (!args || !*args) {
        printf("usage: unsafe on|off\n");
        return;
    }
    str_lower(args);
    bool enable = strcmp(args, "on") == 0;
    if (!enable && strcmp(args, "off") != 0) {
        printf("usage: unsafe on|off\n");
        return;
    }
    if (enable) {
        ESP_LOGW(TAG, "ENABLING allow_unsafe — Mode 04 may be allowed; Mode 08 still blocked");
        printf("WARNING: allow_unsafe ON (Mode 08 still blocked)\n");
    }
    profile_store_set_allow_unsafe(enable);
    printf("allow_unsafe=%s\n", enable ? "true" : "false");
}

/** @brief Print runtime metrics JSON snapshot. */
static void cmd_metrics(void)
{
    char metrics[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(metrics, sizeof(metrics));
    printf("%s\n", metrics);
}

/** @brief fleet hosts | fleet ingest <hex> — loopback Zigbee TLV testing. */
static void cmd_fleet(char *args)
{
    if (args == NULL) {
        args = "";
    }
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "hosts", 5) == 0) {
        static fleet_registry_snapshot_t snap;
        if (!host_registry_snapshot(&snap)) {
            printf("registry unavailable\n");
            return;
        }
        printf("host_count=%u joined=%lu\n", snap.host_count,
               (unsigned long)snap.joined_count);
        for (uint8_t i = 0; i < snap.host_count; i++) {
            const fleet_registry_host_t *h = &snap.hosts[i];
            printf("  %s type=%s id=%u link=%d last=%llu\n", h->device_id, h->host_type,
                   (unsigned)h->host_type_id, (int)h->link_ok,
                   (unsigned long long)h->last_seen_ms);
            for (uint8_t r = 0; r < h->reading_count; r++) {
                if (!h->readings[r].valid) {
                    continue;
                }
                printf("    %s=%.4g %s\n", h->readings[r].key, h->readings[r].value,
                       h->readings[r].unit);
            }
        }
        return;
    }
    if (strncmp(args, "ingest ", 7) == 0) {
        args += 7;
        uint8_t frame[128];
        size_t n = 0;
        while (*args && n < sizeof(frame)) {
            while (*args == ' ') {
                args++;
            }
            if (!isxdigit((unsigned char)args[0]) || !isxdigit((unsigned char)args[1])) {
                break;
            }
            char byte[3] = {args[0], args[1], '\0'};
            frame[n++] = (uint8_t)strtoul(byte, NULL, 16);
            args += 2;
        }
        if (n == 0) {
            printf("usage: fleet ingest <hex bytes>\n");
            return;
        }
        esp_err_t err = transport_zigbee_ingest(frame, n, 0x0001);
        printf("ingest %zu bytes: %s\n", n, esp_err_to_name(err));
        return;
    }
    printf("usage: fleet hosts | fleet ingest <hex>\n");
}

/** @brief Route a parsed console verb to its command handler. */
static void dispatch(char *verb, char *args)
{
    str_lower(verb);

    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "config") == 0) {
        cmd_config();
    } else if (strcmp(verb, "provision") == 0) {
        cmd_provision(args);
    } else if (strcmp(verb, "status") == 0) {
        cmd_status();
    } else if (strcmp(verb, "cmd") == 0) {
        cmd_cmd(args);
    } else if (strcmp(verb, "vin") == 0) {
        cmd_vin();
    } else if (strcmp(verb, "dtc") == 0) {
        cmd_dtc(args);
    } else if (strcmp(verb, "profiles") == 0) {
        cmd_profiles();
    } else if (strcmp(verb, "profile") == 0) {
        cmd_profile(args);
    } else if (strcmp(verb, "telemetry") == 0) {
        cmd_telemetry(args);
    } else if (strcmp(verb, "unsafe") == 0) {
        cmd_unsafe(args);
    } else if (strcmp(verb, "metrics") == 0) {
        cmd_metrics();
    } else if (strcmp(verb, "lte") == 0) {
        cmd_lte(args);
    } else if (strcmp(verb, "uplink") == 0) {
        cmd_uplink(args);
    } else if (strcmp(verb, "ota") == 0) {
        cmd_ota(args);
    } else if (strcmp(verb, "fleet") == 0) {
        cmd_fleet(args);
    } else {
        printf("unknown command: %s (type 'help')\n", verb);
    }
}

/** @brief Read one echoed line from USB-Serial/JTAG (blocks until CR/LF). */
static int read_line_usb(char *buf, size_t buflen)
{
    if (buflen < 2) {
        return -1;
    }
    size_t n = 0;
    while (n + 1 < buflen) {
        uint8_t ch = 0;
        int got = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (got <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        buf[n++] = (char)ch;
        /* echo for interactive use */
        usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(20));
    }
    buf[n] = '\0';
    const char crlf[] = "\r\n";
    usb_serial_jtag_write_bytes(crlf, 2, pdMS_TO_TICKS(20));
    return (int)n;
}

/** @brief USB console FreeRTOS task: prompt, read line, dispatch commands. */
static void console_task(void *arg)
{
    (void)arg;

    /* Ensure USB-Serial/JTAG driver is installed for byte reads. If the IDF
     * console already installed it, INVALID_STATE is fine. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install: %s (falling back to fgets)",
                 esp_err_to_name(err));
    }

    char line[LINE_BUF_LEN];
    printf("\nCAN OBD console ready. Type help.\n");
    fflush(stdout);

    while (1) {
        const char *prompt = "obd> ";
        usb_serial_jtag_write_bytes(prompt, strlen(prompt), pdMS_TO_TICKS(50));
        printf("%s", prompt);
        fflush(stdout);

        int n = read_line_usb(line, sizeof(line));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char *trimmed = trim(line);
        if (*trimmed == '\0') {
            continue;
        }

        char *verb;
        char *args;
        split_verb_args(trimmed, &verb, &args);
        dispatch(verb, args);
    }
}

/** @brief Start the USB console task once (no-op if already running). */
esp_err_t transport_serial_start(void)
{
    if (s_started && s_console_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL,
                                     CONSOLE_TASK_PRIO, &s_console_task_handle);
    if (created != pdPASS) {
        s_console_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "console task started");
    return ESP_OK;
}

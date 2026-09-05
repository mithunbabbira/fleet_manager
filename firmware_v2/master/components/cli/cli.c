#include "cli.h"

#include "can_obd.h"
#include "cli_format.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fleet_tlv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_registry.h"
#include "lte.h"
#include "obd_poller.h"
#include "ota_cloud.h"
#include "ota_flash.h"
#include "store_sd.h"
#include "uplink.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cli";

static void print_help(void)
{
    printf(
        "Commands:\n"
        "  help\n"
        "  status\n"
        "  gps\n"
        "  can status\n"
        "  fleet hosts\n"
        "  fleet demo\n"
        "  device_id <id>\n"
        "  node_id <id>\n"
        "  ota_token <Authorization value or empty>\n"
        "  ota_user <x-nc-system-user-id or empty>\n"
        "  save\n"
        "  ota check\n"
        "  uplink status\n"
        "  uplink once\n"
        "  uplink test\n"
        "  uplink url [<url>]\n"
        "  sd status\n");
}

static void cmd_status(void)
{
    /* Large structs: keep off the CLI task stack (status used to overflow 4K). */
    static ota_flash_status_t flash;
    static ota_cloud_status_t cloud;
    static ota_cloud_config_t cfg;
    static lte_status_t lte;
    static lte_time_t tim;
    static lte_gps_t gps;
    static char line[128];

    memset(&flash, 0, sizeof(flash));
    memset(&cloud, 0, sizeof(cloud));
    memset(&cfg, 0, sizeof(cfg));
    memset(&lte, 0, sizeof(lte));
    memset(&tim, 0, sizeof(tim));
    memset(&gps, 0, sizeof(gps));

    ota_flash_get_status(&flash);
    ota_cloud_get_status(&cloud);
    ota_cloud_get_config(&cfg);
    lte_get_status(&lte);

    printf("fw=%s run=%s update=%s\n", flash.fw_version, flash.running_partition,
           flash.update_partition);
    printf("lte: uart=%s sim=%s reg=%s att=%s csq=%d\n", lte.uart_ok ? "yes" : "no",
           lte.sim_ready ? "yes" : "no", lte.registered ? "yes" : "no",
           lte.attached ? "yes" : "no", lte.csq);
    printf("ota: phase=%d avail=%s err=\"%s\"\n", (int)cloud.phase,
           cloud.update_available ? "yes" : "no", cloud.error);
    printf("cfg: device_id=%s token=%s user=%s\n", cfg.device_id,
           cfg.auth_token[0] ? "(set)" : "(empty)",
           cfg.system_user_id[0] ? cfg.system_user_id : "(empty)");
    (void)lte_time_get(&tim);
    (void)lte_gps_get(&gps);
    if (cli_format_time_line(&tim, line, sizeof(line)) > 0) {
        printf("%s\n", line);
    }
    if (cli_format_gps_line(&gps, line, sizeof(line)) > 0) {
        printf("%s\n", line);
    }
    static uplink_status_t up;
    static char nid[40];
    memset(&up, 0, sizeof(up));
    nid[0] = '\0';
    (void)uplink_get_status(&up);
    (void)uplink_get_node_id(nid, sizeof(nid));
    printf("uplink: en=%s node=%s http=%d ok=%lu fail=%lu err=\"%s\"\n",
           up.enabled ? "yes" : "no", nid[0] ? nid : "(empty)", up.last_http_status,
           (unsigned long)up.send_ok, (unsigned long)up.send_fail,
           up.last_error[0] ? up.last_error : "");

    char proto[48];
    can_obd_get_protocol(proto, sizeof(proto));
    printf("can: mcp=%s ready=%s protocol=%s\n", can_obd_mcp_present() ? "yes" : "no",
           can_obd_is_ready() ? "yes" : "no", proto);

    static obd_poller_snapshot_t obd;
    memset(&obd, 0, sizeof(obd));
    if (obd_poller_get_snapshot(&obd) == ESP_OK) {
        printf("obd: en=%s fresh=%s rpm=%s speed=%s coolant=%s throttle=%s ok=%llu fail=%llu\n",
               obd.enabled ? "yes" : "no", obd_poller_has_fresh_pid(&obd) ? "yes" : "no",
               (obd.rpm.valid && obd.rpm.ok) ? "ok" : "-",
               (obd.speed.valid && obd.speed.ok) ? "ok" : "-",
               (obd.coolant.valid && obd.coolant.ok) ? "ok" : "-",
               (obd.throttle.valid && obd.throttle.ok) ? "ok" : "-",
               (unsigned long long)obd.cmds_ok, (unsigned long long)obd.cmds_fail);
    }
}

static void cmd_gps(void)
{
    static lte_gps_t gps;
    static char line[128];
    memset(&gps, 0, sizeof(gps));
    (void)lte_gps_get(&gps);
    if (cli_format_gps_line(&gps, line, sizeof(line)) > 0) {
        printf("%s\n", line);
    }
}

static void cmd_fleet_hosts(void)
{
    static fleet_registry_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (!host_registry_snapshot(&snap)) {
        printf("registry unavailable\n");
        return;
    }
    printf("host_count=%u joined=%lu\n", snap.host_count, (unsigned long)snap.joined_count);
    for (uint8_t i = 0; i < snap.host_count; i++) {
        const fleet_registry_host_t *h = &snap.hosts[i];
        printf("  %s type=%s id=%u node=%s schema=%s link=%d last=%llu\n", h->device_id,
               h->host_type, (unsigned)h->host_type_id, h->node_id[0] ? h->node_id : "-",
               h->schema_id[0] ? h->schema_id : "-", (int)h->link_ok,
               (unsigned long long)h->last_seen_ms);
        for (uint8_t r = 0; r < h->reading_count; r++) {
            if (!h->readings[r].valid) {
                continue;
            }
            printf("    %s=%.4g %s\n", h->readings[r].key, h->readings[r].value,
                   h->readings[r].unit);
        }
    }
}

/** Lab: inject a synthetic UL212 REPORT into the registry (no radio / no car). */
static void cmd_fleet_demo(void)
{
    fleet_encode_input_t in;
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    snprintf(in.header.device_id, sizeof(in.header.device_id), "%s", "ul212-lab-001");
    snprintf(in.header.node_id, sizeof(in.header.node_id), "%s", "node-ul212-lab-001");
    snprintf(in.header.schema_id, sizeof(in.header.schema_id), "%s", "1088");
    snprintf(in.header.host_type, sizeof(in.header.host_type), "%s", "ul212_ble_fetch");
    in.header.host_type_id = 1;
    in.header.seq = 1;
    in.header.ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    in.header.status = FLEET_STATUS_READING_VALID | FLEET_STATUS_SENSOR_CONNECTED;
    in.reading_count = 1;
    in.readings[0].tlv_id = 16; /* height_mm */
    in.readings[0].type = FLEET_VAL_FLOAT;
    in.readings[0].value.f32 = 123.4f;
    in.readings[0].valid = true;

    uint8_t frame[FLEET_TLV_MAX_FRAME];
    int n = fleet_tlv_encode(&in, frame, sizeof(frame));
    if (n <= 0) {
        printf("fleet demo: encode failed\n");
        return;
    }
    int rc = host_registry_ingest_frame_ex(frame, (size_t)n, in.header.ts_ms, 0x42, false);
    printf("fleet demo: ingest rc=%d (ul212-lab-001 height_mm=123.4)\n", rc);
}

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }

    if (strcmp(line, "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(line, "status") == 0) {
        cmd_status();
        return;
    }
    if (strcmp(line, "gps") == 0) {
        cmd_gps();
        return;
    }
    if (strcmp(line, "can status") == 0 || strcmp(line, "can") == 0) {
        char proto[48];
        can_obd_get_protocol(proto, sizeof(proto));
        printf("can: mcp=%s ready=%s protocol=%s\n", can_obd_mcp_present() ? "yes" : "no",
               can_obd_is_ready() ? "yes" : "no", proto);
        printf("note: mcp=chip detect (no car); ready=ECU answered 0100\n");
        return;
    }
    if (strcmp(line, "fleet hosts") == 0 || strcmp(line, "fleet") == 0) {
        cmd_fleet_hosts();
        return;
    }
    if (strcmp(line, "fleet demo") == 0) {
        cmd_fleet_demo();
        return;
    }
    if (strcmp(line, "uplink status") == 0) {
        static uplink_status_t up;
        memset(&up, 0, sizeof(up));
        if (uplink_get_status(&up) != ESP_OK) {
            printf("uplink status unavailable\n");
            return;
        }
        printf("uplink: en=%s url=%s node=%s http=%d ok=%lu fail=%lu skip_id=%lu skip_gps=%lu "
               "skip_obd=%lu skip_gate=%lu q_enq=%lu q_drn=%lu q_depth=%lu err=\"%s\"\n",
               up.enabled ? "yes" : "no", up.url, up.node_id[0] ? up.node_id : "(empty)",
               up.last_http_status, (unsigned long)up.send_ok, (unsigned long)up.send_fail,
               (unsigned long)up.skip_no_id, (unsigned long)up.skip_no_gps,
               (unsigned long)up.skip_no_obd, (unsigned long)up.skip_gate,
               (unsigned long)up.queue_enqueued, (unsigned long)up.queue_drained,
               (unsigned long)up.queue_depth, up.last_error[0] ? up.last_error : "");
        return;
    }
    if (strcmp(line, "sd status") == 0 || strcmp(line, "sd") == 0) {
        store_sd_status_t sd;
        memset(&sd, 0, sizeof(sd));
        if (store_sd_get_status(&sd) != ESP_OK) {
            printf("sd status unavailable\n");
            return;
        }
        printf("sd: mounted=%s depth=%lu pending_bytes=%llu err=\"%s\"\n",
               sd.mounted ? "yes" : "no", (unsigned long)sd.depth,
               (unsigned long long)sd.pending_bytes, sd.last_error[0] ? sd.last_error : "");
        return;
    }
    if (strcmp(line, "uplink once") == 0) {
        esp_err_t err = uplink_once();
        printf("uplink once: %s\n", esp_err_to_name(err));
        return;
    }
    if (strcmp(line, "uplink test") == 0) {
        esp_err_t err = uplink_lab_post();
        printf("uplink test: %s\n", esp_err_to_name(err));
        return;
    }
    if (strcmp(line, "uplink url") == 0) {
        static uplink_status_t up;
        memset(&up, 0, sizeof(up));
        if (uplink_get_status(&up) != ESP_OK) {
            printf("uplink url unavailable\n");
            return;
        }
        printf("uplink url=%s\n", up.url[0] ? up.url : "(empty)");
        return;
    }
    if (strncmp(line, "uplink url ", 11) == 0) {
        esp_err_t err = uplink_set_post_url(line + 11);
        printf("uplink url set (%s)\n", esp_err_to_name(err));
        return;
    }
    if (strncmp(line, "node_id ", 8) == 0) {
        esp_err_t err = uplink_set_node_id(line + 8);
        printf("node_id=%s (%s)\n", line + 8, esp_err_to_name(err));
        return;
    }
    static ota_cloud_config_t cfg;

    if (strcmp(line, "save") == 0) {
        if (ota_cloud_get_config(&cfg) == ESP_OK && ota_cloud_set_config(&cfg) == ESP_OK) {
            printf("saved\n");
        } else {
            printf("save failed\n");
        }
        return;
    }
    if (strcmp(line, "ota check") == 0) {
        esp_err_t err = ota_cloud_start_background();
        printf("ota check: %s\n", esp_err_to_name(err));
        return;
    }

    if (ota_cloud_get_config(&cfg) != ESP_OK) {
        printf("config unavailable\n");
        return;
    }

    if (strncmp(line, "device_id ", 10) == 0) {
        snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", line + 10);
        ota_cloud_set_config(&cfg);
        printf("device_id=%s (saved)\n", cfg.device_id);
        return;
    }
    if (strncmp(line, "ota_token ", 10) == 0) {
        snprintf(cfg.auth_token, sizeof(cfg.auth_token), "%s", line + 10);
        ota_cloud_set_config(&cfg);
        printf("ota_token updated (saved)\n");
        return;
    }
    if (strcmp(line, "ota_token") == 0) {
        cfg.auth_token[0] = '\0';
        ota_cloud_set_config(&cfg);
        printf("ota_token cleared\n");
        return;
    }
    if (strncmp(line, "ota_user ", 9) == 0) {
        snprintf(cfg.system_user_id, sizeof(cfg.system_user_id), "%s", line + 9);
        ota_cloud_set_config(&cfg);
        printf("ota_user=%s (saved)\n", cfg.system_user_id);
        return;
    }
    if (strcmp(line, "ota_user") == 0) {
        cfg.system_user_id[0] = '\0';
        ota_cloud_set_config(&cfg);
        printf("ota_user cleared\n");
        return;
    }

    printf("unknown: %s (type help)\n", line);
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[256];
    size_t used = 0;
    printf("\nFirmware v2 master console. Type help.\n");
    printf("> ");
    fflush(stdout);
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[used] = '\0';
            handle_line(line);
            used = 0;
            printf("> ");
            fflush(stdout);
            continue;
        }
        if (used + 1 < sizeof(line)) {
            line[used++] = (char)c;
        }
    }
}

esp_err_t cli_start(void)
{
    /* HTTP POST / uplink_once needs headroom beyond status/fleet statics. */
    if (xTaskCreate(cli_task, "cli", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cli task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

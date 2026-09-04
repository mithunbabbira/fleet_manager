#include "cli.h"

#include "cli_format.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lte.h"
#include "ota_cloud.h"
#include "ota_flash.h"

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
        "  device_id <id>\n"
        "  ota_token <Authorization value or empty>\n"
        "  ota_user <x-nc-system-user-id or empty>\n"
        "  save\n"
        "  ota check\n");
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
    if (xTaskCreate(cli_task, "cli", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cli task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

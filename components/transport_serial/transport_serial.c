#include "transport_serial.h"

#include "can_obd.h"
#include "cmd_policy.h"
#include "net_lte.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"
#include "telemetry_uplink.h"

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

#define CONSOLE_TASK_STACK   6144
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

static void str_lower(char *s)
{
    for (; *s != '\0'; ++s) {
        *s = (char)tolower((unsigned char)*s);
    }
}

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

static void cmd_help(void)
{
    printf(
        "Commands:\n"
        "  help\n"
        "  status\n"
        "  cmd <OBD>\n"
        "  profiles\n"
        "  profile <name>\n"
        "  telemetry on|off\n"
        "  unsafe on|off\n"
        "  metrics\n"
        "  lte [reconnect|test]\n"
        "  uplink [on|off|now]\n");
}

static void cmd_uplink(char *args)
{
    if (args && args[0]) {
        str_lower(args);
        if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
            telemetry_uplink_config_t cfg;
            if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
                printf("uplink: config unavailable\n");
                return;
            }
            cfg.enabled = (strcmp(args, "on") == 0);
            esp_err_t err = telemetry_uplink_set_config(&cfg);
            printf("uplink %s: %s\n", args, esp_err_to_name(err));
            return;
        }
        if (strcmp(args, "now") == 0) {
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
    printf("        url=%s schemaId=%s\n", st.url, st.schema_id);
}

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

static void cmd_metrics(void)
{
    char metrics[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(metrics, sizeof(metrics));
    printf("%s\n", metrics);
}

static void dispatch(char *verb, char *args)
{
    str_lower(verb);

    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "status") == 0) {
        cmd_status();
    } else if (strcmp(verb, "cmd") == 0) {
        cmd_cmd(args);
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
    } else {
        printf("unknown command: %s (type 'help')\n", verb);
    }
}

/* Read one line from USB-Serial/JTAG using the driver API (more reliable than
 * fgets on secondary/primary USB console). Blocks until CR/LF or buffer full. */
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

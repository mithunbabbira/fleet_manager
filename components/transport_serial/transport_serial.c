#include "transport_serial.h"

#include "ble_elm.h"
#include "cmd_policy.h"
#include "elm327_client.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "transport_serial";

#define CONSOLE_TASK_STACK   6144
#define CONSOLE_TASK_PRIO    4
#define TELEMETRY_TASK_STACK 3072
#define TELEMETRY_TASK_PRIO  3

#define LINE_BUF_LEN     256
#define RESP_BUF_LEN     256
#define METRICS_BUF_LEN  768
#define MAX_SCAN_DEVICES 16
#define MAX_PROFILE_NAMES 16

#define TELEMETRY_POLL_MS  500
#define BLE_READY_WAIT_MS  8000
#define BLE_READY_POLL_MS  200

static bool s_started;
static TaskHandle_t s_console_task_handle;

static QueueHandle_t s_telemetry_queue;
static TaskHandle_t s_telemetry_task_handle;
static volatile bool s_telemetry_stop_requested;

static ble_elm_device_t s_last_devices[MAX_SCAN_DEVICES];
static int s_last_device_count;

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

/* Splits an already-trimmed line into a NUL-terminated verb and the
 * (left-trimmed) remainder of the line. `line` is modified in place. */
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

static void format_addr(const uint8_t addr[6], char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
              addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/* ---- init-sequence helper --------------------------------------------- */

static void run_active_init_sequence(void)
{
    obd_profile_t profile;
    esp_err_t err = profile_store_get_active(&profile);
    if (err != ESP_OK) {
        printf("failed to load active profile: %s\n", esp_err_to_name(err));
        return;
    }
    if (profile.init_at_count <= 0) {
        printf("profile \"%s\" has no init sequence; skipping\n", profile.name);
        return;
    }

    err = elm327_client_run_init_sequence(profile.init_at, profile.init_at_count);
    if (err == ESP_OK) {
        printf("init sequence OK (profile \"%s\")\n", profile.name);
    } else {
        printf("init sequence failed: %s\n", esp_err_to_name(err));
    }
}

/* ---- telemetry dump task ---------------------------------------------- */

static void print_telemetry_msg(const telemetry_msg_t *msg)
{
    switch (msg->type) {
    case TELEMETRY_PID_SAMPLE:
        if (msg->pid_sample.ok) {
            printf("[telemetry] %-8s %-16s = %.2f %s (raw=%s)\n", msg->pid_sample.cmd,
                   msg->pid_sample.name, msg->pid_sample.value, msg->pid_sample.unit,
                   msg->pid_sample.raw_hex);
        } else {
            printf("[telemetry] %-8s %-16s decode failed\n", msg->pid_sample.cmd,
                   msg->pid_sample.name);
        }
        break;
    case TELEMETRY_DTC_LIST:
        printf("[telemetry] DTCs (%d):", msg->dtc_list.count);
        for (int i = 0; i < msg->dtc_list.count; ++i) {
            printf(" %s", msg->dtc_list.codes[i]);
        }
        printf("\n");
        break;
    case TELEMETRY_ELM_EVENT: {
        const char *kind = "unknown";
        switch (msg->elm_event.kind) {
        case TELEMETRY_ELM_EVENT_CONNECTED: kind = "connected"; break;
        case TELEMETRY_ELM_EVENT_DISCONNECTED: kind = "disconnected"; break;
        case TELEMETRY_ELM_EVENT_INIT_OK: kind = "init_ok"; break;
        case TELEMETRY_ELM_EVENT_INIT_FAIL: kind = "init_fail"; break;
        default: break;
        }
        printf("[telemetry] elm event: %s\n", kind);
        break;
    }
    case TELEMETRY_ERROR:
        printf("[telemetry] ERROR cmd=%s: %s\n", msg->error.cmd, msg->error.message);
        break;
    default:
        break;
    }
}

static void telemetry_dump_task(void *arg)
{
    (void)arg;
    telemetry_msg_t msg;

    ESP_LOGI(TAG, "telemetry dump task started");
    while (!s_telemetry_stop_requested) {
        if (xQueueReceive(s_telemetry_queue, &msg, pdMS_TO_TICKS(TELEMETRY_POLL_MS)) == pdTRUE) {
            print_telemetry_msg(&msg);
        }
    }

    s_telemetry_task_handle = NULL;
    ESP_LOGI(TAG, "telemetry dump task stopped");
    vTaskDelete(NULL);
}

/* ---- command handlers --------------------------------------------------*/

static void cmd_help(void)
{
    printf(
        "Commands:\n"
        "  help                 list commands\n"
        "  status               BLE connected? / ELM ready? / active profile / metrics\n"
        "  scan                 scan for BLE ELM327 adapters and print devices\n"
        "  devices              print the results of the last scan\n"
        "  select <idx>         bond + connect to device <idx> from the last scan\n"
        "  cmd <AT/OBD>          send a raw AT/OBD command through the safety gate\n"
        "  profiles             list stored profiles (active one marked with *)\n"
        "  profile <name>       set the active profile and reload the poller\n"
        "  telemetry on|off     stream live telemetry samples to the console\n"
        "  unsafe on|off        allow/deny unsafe (write-capable) OBD commands\n"
        "  metrics              print runtime counters as JSON\n");
}

static void cmd_status(void)
{
    bool connected = ble_elm_is_connected();
    bool ready = elm327_client_is_ready();

    obd_profile_t profile;
    esp_err_t perr = profile_store_get_active(&profile);

    printf("BLE connected: %s\n", connected ? "yes" : "no");
    printf("ELM ready:     %s\n", ready ? "yes" : "no");
    if (perr == ESP_OK) {
        printf("Active profile: %s (%d items, %d init steps)\n", profile.name,
               profile.item_count, profile.init_at_count);
    } else {
        printf("Active profile: <none> (%s)\n", esp_err_to_name(perr));
    }

    printf("Metrics: cmds_ok=%llu cmds_fail=%llu blocked_cmds=%llu telemetry_drops=%llu\n",
           (unsigned long long)sys_runtime_metric_get("cmds_ok"),
           (unsigned long long)sys_runtime_metric_get("cmds_fail"),
           (unsigned long long)sys_runtime_metric_get("blocked_cmds"),
           (unsigned long long)sys_runtime_metric_get("telemetry_drops"));
}

static void cmd_devices(void)
{
    esp_err_t err = ble_elm_get_scan_results(s_last_devices, MAX_SCAN_DEVICES, &s_last_device_count);
    if (err != ESP_OK) {
        printf("failed to read scan results: %s\n", esp_err_to_name(err));
        return;
    }

    if (s_last_device_count == 0) {
        printf("no devices found; run 'scan' first\n");
        return;
    }

    printf("idx  name                              addr               rssi\n");
    for (int i = 0; i < s_last_device_count; ++i) {
        char addr_str[18];
        format_addr(s_last_devices[i].addr, addr_str, sizeof(addr_str));
        printf("%-4d %-33s %-18s %d\n", i, s_last_devices[i].name, addr_str,
               s_last_devices[i].rssi);
    }
}

static void cmd_scan(void)
{
    printf("scanning for %d ms...\n", CONFIG_ELM_BLE_SCAN_MS);
    esp_err_t err = ble_elm_start_scan(CONFIG_ELM_BLE_SCAN_MS);
    if (err != ESP_OK) {
        printf("scan failed: %s (has ble_elm_init() been called?)\n", esp_err_to_name(err));
        return;
    }
    cmd_devices();
}

static void cmd_select(const char *args)
{
    if (*args == '\0') {
        printf("usage: select <idx>  (see 'devices')\n");
        return;
    }

    int idx = atoi(args);
    if (idx < 0 || idx >= s_last_device_count) {
        printf("no such device index %d; run 'scan' or 'devices' first\n", idx);
        return;
    }

    ble_elm_device_t dev = s_last_devices[idx];
    char addr_str[18];
    format_addr(dev.addr, addr_str, sizeof(addr_str));

    ble_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    memcpy(bond.addr, dev.addr, sizeof(bond.addr));
    bond.addr_set = true;
    snprintf(bond.name, sizeof(bond.name), "%s", dev.name);
    esp_err_t err = profile_store_set_bond(&bond);
    if (err != ESP_OK) {
        printf("warning: failed to persist bond: %s\n", esp_err_to_name(err));
    }

    printf("connecting to %s (%s)...\n", dev.name, addr_str);
    err = ble_elm_connect_addr(dev.addr);
    if (err != ESP_OK) {
        printf("connect failed: %s\n", esp_err_to_name(err));
        return;
    }

    elm327_client_set_transport(ble_elm_get_transport());

    int waited_ms = 0;
    while (!elm327_client_is_ready() && waited_ms < BLE_READY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(BLE_READY_POLL_MS));
        waited_ms += BLE_READY_POLL_MS;
    }

    if (!elm327_client_is_ready()) {
        printf("connected, but GATT service not ready after %d ms; "
               "try 'select %d' again or 'cmd ATZ' manually\n", BLE_READY_WAIT_MS, idx);
        return;
    }

    printf("connected; running init sequence...\n");
    run_active_init_sequence();
}

static void cmd_cmd(const char *args)
{
    if (*args == '\0') {
        printf("usage: cmd <AT/OBD command>\n");
        return;
    }

    char resp[RESP_BUF_LEN];
    resp[0] = '\0';
    esp_err_t err = obd_poller_submit_raw(args, resp, sizeof(resp), 0);
    if (err == ESP_OK) {
        printf("%s\n", resp);
    } else if (err == ESP_ERR_NOT_ALLOWED) {
        printf("blocked by policy: %s\n", resp[0] != '\0' ? resp : "command denied by cmd_policy");
    } else {
        printf("error: %s\n", esp_err_to_name(err));
    }
}

static void cmd_profiles(void)
{
    char names[MAX_PROFILE_NAMES][32];
    int count = 0;
    esp_err_t err = profile_store_list(names, MAX_PROFILE_NAMES, &count);
    if (err != ESP_OK) {
        printf("failed to list profiles: %s\n", esp_err_to_name(err));
        return;
    }

    obd_profile_t active;
    bool have_active = profile_store_get_active(&active) == ESP_OK;

    for (int i = 0; i < count; ++i) {
        bool is_active = have_active && strcmp(names[i], active.name) == 0;
        printf("%s%s\n", is_active ? "* " : "  ", names[i]);
    }
}

static void cmd_profile(const char *args)
{
    if (*args == '\0') {
        printf("usage: profile <name>  (see 'profiles')\n");
        return;
    }

    esp_err_t err = profile_store_set_active(args);
    if (err != ESP_OK) {
        printf("failed to switch profile: %s\n", esp_err_to_name(err));
        return;
    }

    err = obd_poller_reload_active_profile();
    if (err != ESP_OK) {
        printf("warning: poller reload failed: %s\n", esp_err_to_name(err));
    }

    printf("active profile set to \"%s\"\n", args);

    if (elm327_client_is_ready()) {
        printf("re-running init sequence for new profile...\n");
        run_active_init_sequence();
    }
}

static void cmd_telemetry(const char *args)
{
    char verb[8];
    snprintf(verb, sizeof(verb), "%s", args);
    str_lower(verb);

    if (strcmp(verb, "on") == 0) {
        if (s_telemetry_queue == NULL) {
            esp_err_t err = telemetry_subscribe(&s_telemetry_queue, TELEMETRY_MASK_ALL);
            if (err != ESP_OK) {
                printf("failed to subscribe to telemetry: %s\n", esp_err_to_name(err));
                return;
            }
        }
        if (s_telemetry_task_handle != NULL) {
            printf("telemetry streaming already on\n");
            return;
        }
        s_telemetry_stop_requested = false;
        BaseType_t created = xTaskCreate(telemetry_dump_task, "telemetry_dump",
                                         TELEMETRY_TASK_STACK, NULL, TELEMETRY_TASK_PRIO,
                                         &s_telemetry_task_handle);
        if (created != pdPASS) {
            s_telemetry_task_handle = NULL;
            printf("failed to start telemetry dump task\n");
            return;
        }
        printf("telemetry streaming ON\n");
    } else if (strcmp(verb, "off") == 0) {
        if (s_telemetry_task_handle == NULL) {
            printf("telemetry streaming already off\n");
            return;
        }
        s_telemetry_stop_requested = true;
        printf("telemetry streaming OFF\n");
    } else {
        printf("usage: telemetry on|off\n");
    }
}

static void cmd_unsafe(const char *args)
{
    char verb[8];
    snprintf(verb, sizeof(verb), "%s", args);
    str_lower(verb);

    if (strcmp(verb, "on") == 0) {
        esp_err_t err = profile_store_set_allow_unsafe(true);
        if (err != ESP_OK) {
            printf("failed to enable unsafe mode: %s\n", esp_err_to_name(err));
            return;
        }
        ESP_LOGW(TAG, "!!! UNSAFE MODE ENABLED: mode-04/write-capable OBD commands may now "
                       "be permitted (mode 08 remains blocked). Use with extreme caution !!!");
        printf("unsafe mode ENABLED\n");
    } else if (strcmp(verb, "off") == 0) {
        esp_err_t err = profile_store_set_allow_unsafe(false);
        if (err != ESP_OK) {
            printf("failed to disable unsafe mode: %s\n", esp_err_to_name(err));
            return;
        }
        printf("unsafe mode disabled\n");
    } else {
        printf("usage: unsafe on|off\n");
    }
}

static void cmd_metrics(void)
{
    char buf[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(buf, sizeof(buf));
    printf("%s\n", buf);
}

/* ---- dispatch + task --------------------------------------------------- */

static void dispatch(char *verb, char *args)
{
    str_lower(verb);

    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "status") == 0) {
        cmd_status();
    } else if (strcmp(verb, "scan") == 0) {
        cmd_scan();
    } else if (strcmp(verb, "devices") == 0) {
        cmd_devices();
    } else if (strcmp(verb, "select") == 0) {
        cmd_select(args);
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
    } else {
        printf("unknown command: %s (type 'help')\n", verb);
    }
}

static void console_task(void *arg)
{
    (void)arg;

    /* Unbuffered stdin so fgets() sees each character as it arrives on the
     * console UART / USB-Serial-JTAG VFS driver instead of waiting for a
     * full stdio buffer. */
    setvbuf(stdin, NULL, _IONBF, 0);

    char line[LINE_BUF_LEN];

    printf("\nELM327 console ready. Type help.\n");

    while (1) {
        printf("elm> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* No console driver / EOF: avoid a hot spin. */
            vTaskDelay(pdMS_TO_TICKS(50));
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

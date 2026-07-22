#include "transport_serial.h"

#include "ble_elm.h"
#include "cmd_policy.h"
#include "elm327_client.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"

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
        obd_poller_set_enabled(true);
        return;
    }

    obd_poller_set_enabled(false);
    err = elm327_client_run_init_sequence(profile.init_at, profile.init_at_count);
    if (err == ESP_OK) {
        printf("init sequence OK (profile \"%s\")\n", profile.name);
        char resp[128];
        esp_err_t pid_err = elm327_client_transact("0100", resp, sizeof(resp), 15000);
        printf("0100 -> %s (%s)\n", resp, esp_err_to_name(pid_err));
        obd_poller_set_enabled(true);
    } else {
        printf("init sequence failed: %s\n", esp_err_to_name(err));
        obd_poller_set_enabled(false);
    }
}

static bool wait_elm_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (elm327_client_is_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(BLE_READY_POLL_MS));
        waited += BLE_READY_POLL_MS;
    }
    return elm327_client_is_ready();
}

/* ---- commands --------------------------------------------------------- */

static void cmd_help(void)
{
    printf(
        "Commands:\n"
        "  help\n"
        "  status\n"
        "  scan\n"
        "  devices\n"
        "  select <idx>\n"
        "  unbond\n"
        "  cmd <AT/OBD>\n"
        "  profiles\n"
        "  profile <name>\n"
        "  init\n"
        "  telemetry on|off\n"
        "  unsafe on|off\n"
        "  metrics\n");
}

static void cmd_status(void)
{
    obd_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    profile_store_get_active(&profile);

    printf("ble_connected=%s elm_ready=%s poller=%s profile=%s items=%d\n",
           ble_elm_is_connected() ? "yes" : "no",
           elm327_client_is_ready() ? "yes" : "no",
           obd_poller_is_enabled() ? "on" : "paused",
           profile.name[0] ? profile.name : "(none)",
           profile.item_count);

    char metrics[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(metrics, sizeof(metrics));
    printf("metrics=%s\n", metrics);
}

static void cmd_scan(void)
{
    printf("scanning BLE (%d ms)...\n", CONFIG_ELM_BLE_SCAN_MS);
    esp_err_t err = ble_elm_start_scan(CONFIG_ELM_BLE_SCAN_MS);
    if (err != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        return;
    }
    s_last_device_count = 0;
    err = ble_elm_get_scan_results(s_last_devices, MAX_SCAN_DEVICES, &s_last_device_count);
    if (err != ESP_OK) {
        printf("get results failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("found %d device(s)\n", s_last_device_count);
    for (int i = 0; i < s_last_device_count; ++i) {
        char addr[18];
        format_addr(s_last_devices[i].addr, addr, sizeof(addr));
        printf("  [%d] %s  rssi=%d  name=\"%s\"\n", i, addr, s_last_devices[i].rssi,
               s_last_devices[i].name[0] ? s_last_devices[i].name : "(none)");
    }
}

static void cmd_devices(void)
{
    if (s_last_device_count <= 0) {
        ble_elm_get_scan_results(s_last_devices, MAX_SCAN_DEVICES, &s_last_device_count);
    }
    printf("%d device(s)\n", s_last_device_count);
    for (int i = 0; i < s_last_device_count; ++i) {
        char addr[18];
        format_addr(s_last_devices[i].addr, addr, sizeof(addr));
        printf("  [%d] %s  rssi=%d  name=\"%s\"\n", i, addr, s_last_devices[i].rssi,
               s_last_devices[i].name[0] ? s_last_devices[i].name : "(none)");
    }
}

static void cmd_select(char *args)
{
    if (!args || !*args) {
        printf("usage: select <idx>\n");
        return;
    }
    int idx = atoi(args);
    if (s_last_device_count <= 0) {
        ble_elm_get_scan_results(s_last_devices, MAX_SCAN_DEVICES, &s_last_device_count);
    }
    if (idx < 0 || idx >= s_last_device_count) {
        printf("invalid index %d (have %d)\n", idx, s_last_device_count);
        return;
    }

    ble_elm_device_t *dev = &s_last_devices[idx];
    ble_bond_t bond = {0};
    memcpy(bond.addr, dev->addr, 6);
    bond.addr_set = true;
    if (dev->name[0]) {
        snprintf(bond.name, sizeof(bond.name), "%s", dev->name);
    } else {
        /* Many clones (e.g. MODAXE OBDII) advertise without a local name. */
        snprintf(bond.name, sizeof(bond.name), "%s", "MODAXE OBDII");
    }
    profile_store_set_bond(&bond);

    char addr[18];
    format_addr(dev->addr, addr, sizeof(addr));
    printf("connecting to %s \"%s\"...\n", addr, bond.name);

    esp_err_t err = ble_elm_connect_addr(dev->addr);
    if (err != ESP_OK) {
        printf("connect failed: %s\n", esp_err_to_name(err));
        return;
    }

    elm327_client_set_transport(ble_elm_get_transport());
    if (!wait_elm_ready(BLE_READY_WAIT_MS)) {
        printf("GATT not ready yet; try again shortly\n");
        return;
    }
    run_active_init_sequence();
    ble_elm_start_auto_reconnect();
    printf("connected and ELM ready\n");
}

static void cmd_unbond(void)
{
    ble_bond_t empty;
    memset(&empty, 0, sizeof(empty));
    esp_err_t err = profile_store_set_bond(&empty);
    ble_elm_clear_peer();
    ble_elm_disconnect();
    printf("bond cleared%s\n", err == ESP_OK ? "" : " (NVS write failed)");
}

static void cmd_cmd(char *args)
{
    if (!args || !*args) {
        printf("usage: cmd <AT/OBD>\n");
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
    if (elm327_client_is_ready()) {
        run_active_init_sequence();
    }
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
    } else if (strcmp(verb, "scan") == 0) {
        cmd_scan();
    } else if (strcmp(verb, "devices") == 0) {
        cmd_devices();
    } else if (strcmp(verb, "select") == 0) {
        cmd_select(args);
    } else if (strcmp(verb, "unbond") == 0) {
        cmd_unbond();
    } else if (strcmp(verb, "cmd") == 0) {
        cmd_cmd(args);
    } else if (strcmp(verb, "profiles") == 0) {
        cmd_profiles();
    } else if (strcmp(verb, "profile") == 0) {
        cmd_profile(args);
    } else if (strcmp(verb, "init") == 0) {
        if (!elm327_client_is_ready()) {
            printf("ELM not ready; connect first (select)\n");
        } else {
            run_active_init_sequence();
        }
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
    printf("\nELM327 console ready. Type help.\n");
    fflush(stdout);

    while (1) {
        const char *prompt = "elm> ";
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

#include "transport_http.h"

#include "http_api.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "transport_http";

#define HTTP_MAX_URI_HANDLERS 28

static bool s_started;
static httpd_handle_t s_server;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) {
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " joined, aid=%d", MAC2STR(e->mac), e->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " left, aid=%d", MAC2STR(e->mac), e->aid);
    }
}

/* esp_netif_init()/esp_event_loop_create_default() may already have been
 * called by another subsystem (e.g. NimBLE's controller bring-up path on
 * some IDF versions); ESP_ERR_INVALID_STATE from either just means "already
 * done" and is not a real failure here. */
static esp_err_t init_netif_and_event_loop(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t start_softap(void)
{
    esp_err_t err = init_netif_and_event_loop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif/event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGW(TAG, "esp_netif_create_default_wifi_ap: netif already exists?");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                                               NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi event handler register failed: %s", esp_err_to_name(err));
    }

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", CONFIG_ELM_SOFTAP_SSID);
    wifi_config.ap.ssid_len = (uint8_t)strlen(CONFIG_ELM_SOFTAP_SSID);
    snprintf((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), "%s",
             CONFIG_ELM_SOFTAP_PASS);
    if (strlen(CONFIG_ELM_SOFTAP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP up: ssid=\"%s\" channel=1 max_conn=4", CONFIG_ELM_SOFTAP_SSID);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    config.lru_purge_enable = true;
    /* Default 4KB stack overflows on /api/status + cJSON (phone SoftAP crash). */
    config.stack_size = 12288;
    /* Protocol auto-detect (ATSP0 + lock) can take two long ELM inits. */
    config.recv_wait_timeout = 45;
    config.send_wait_timeout = 45;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = http_api_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_api_register failed: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t transport_http_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = start_softap();
    if (err != ESP_OK) {
        return err;
    }

    err = start_httpd();
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    return ESP_OK;
}

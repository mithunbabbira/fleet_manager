/*
 * Zigbee coordinator on the fleet master (ESP-IDF).
 *
 * Flow:
 *   1. Form an open network on CONFIG_FLEET_ZIGBEE_CHANNEL.
 *   2. Keep permit-join open so hosts can (re)join after either side reboots.
 *   3. On custom-cluster RX → transport_zigbee_ingest() → host_registry.
 *
 * Security: open network only (lab / dev). No install codes.
 */

#include "fleet_zigbee_cluster.h"
#include "transport_zigbee.h"

#include "sdkconfig.h"

#if CONFIG_FLEET_ZIGBEE_ENABLE

#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_command.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "zb_radio";

/** Permit-join duration passed to esp_zb_bdb_open_network (seconds). */
#define FLEET_ZB_PERMIT_JOIN_SEC 255
/** Re-open permit join periodically so late-boot hosts can always steer in. */
#define FLEET_ZB_PERMIT_REFRESH_MS 30000

static void retry_formation(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

static void permit_join_refresh_cb(uint8_t param)
{
    (void)param;
    esp_err_t err = esp_zb_bdb_open_network(FLEET_ZB_PERMIT_JOIN_SEC);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "permit join refreshed");
    } else {
        ESP_LOGW(TAG, "permit join refresh failed: %s", esp_err_to_name(err));
    }
    esp_zb_scheduler_alarm((esp_zb_callback_t)permit_join_refresh_cb, 0, FLEET_ZB_PERMIT_REFRESH_MS);
}

static void permit_join_open(void)
{
    esp_err_t err = esp_zb_bdb_open_network(FLEET_ZB_PERMIT_JOIN_SEC);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "permit join open (ch=%d)", CONFIG_FLEET_ZIGBEE_CHANNEL);
    } else {
        ESP_LOGW(TAG, "permit join open failed: %s", esp_err_to_name(err));
    }
    esp_zb_scheduler_alarm((esp_zb_callback_t)permit_join_refresh_cb, 0, FLEET_ZB_PERMIT_REFRESH_MS);
}

/* --- coordinator network config (open join) -------------------------------- */

static esp_zb_cfg_t coordinator_cfg(void)
{
    return (esp_zb_cfg_t){
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {.max_children = 10},
    };
}

/* --- endpoint: custom cluster server --------------------------------------- */

static esp_zb_ep_list_t *make_coordinator_endpoint(void)
{
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(clusters, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *fleet = esp_zb_zcl_attr_list_create(FLEET_ZB_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(clusters, fleet, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *eps = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(eps, clusters, FLEET_ZB_COORD_ENDPOINT, ESP_ZB_AF_HA_PROFILE_ID,
                          ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID);
    return eps;
}

/* --- RX: host TLV frame → ingest ------------------------------------------- */

static esp_err_t on_tlv_command(const esp_zb_zcl_custom_cluster_command_message_t *msg)
{
    if (!msg || msg->info.cluster != FLEET_ZB_CLUSTER_ID) {
        return ESP_OK;
    }
    if (!msg->data.value || msg->data.size == 0) {
        return ESP_OK;
    }

    const uint8_t *raw = (const uint8_t *)msg->data.value;
    uint16_t len = msg->data.size;

    /* ZCL octet-string: [length][payload…] */
    const uint8_t *bytes = raw;
    if (len > 1 && raw[0] + 1U == len) {
        len = raw[0];
        bytes = &raw[1];
    }

    ESP_LOGI(TAG, "TLV %u B from 0x%04x", (unsigned)len, msg->info.src_address.u.short_addr);
    transport_zigbee_ingest(bytes, len, msg->info.src_address.u.short_addr);
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t id, const void *message)
{
    if (id == ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID) {
        return on_tlv_command((const esp_zb_zcl_custom_cluster_command_message_t *)message);
    }
    return ESP_OK;
}

/* --- stack lifecycle (formation + permit join) ----------------------------- */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    const esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*signal_struct->p_app_signal;
    const esp_err_t status = signal_struct->esp_err_status;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status != ESP_OK) {
            ESP_LOGE(TAG, "stack start failed");
            break;
        }
        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "forming network ch=%d", CONFIG_FLEET_ZIGBEE_CHANNEL);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "network up PAN=0x%04x ch=%d", esp_zb_get_pan_id(),
                     esp_zb_get_current_channel());
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            permit_join_open();
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)retry_formation,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "network steering ready");
            permit_join_open();
        } else {
            ESP_LOGW(TAG, "steering failed, retry in 2s");
            esp_zb_scheduler_alarm((esp_zb_callback_t)retry_formation,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const esp_zb_zdo_signal_device_annce_params_t *dev =
            esp_zb_app_signal_get_params(signal_struct->p_app_signal);
        ESP_LOGI(TAG, "host joined 0x%04x", dev->device_short_addr);
        permit_join_open();
        break;
    }

    default:
        break;
    }
}

static void coordinator_task(void *arg)
{
    (void)arg;

    esp_zb_cfg_t cfg = coordinator_cfg();
    esp_zb_init(&cfg);
    esp_zb_device_register(make_coordinator_endpoint());
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(1UL << CONFIG_FLEET_ZIGBEE_CHANNEL);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

esp_err_t transport_zigbee_radio_platform_init(void)
{
    static bool done;
    if (done) {
        return ESP_OK;
    }

    esp_zb_platform_config_t plat = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
    };
    esp_err_t err = esp_zb_platform_config(&plat);
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_wifi_i154_enable();
#endif

    done = true;
    return ESP_OK;
}

esp_err_t transport_zigbee_radio_start(void)
{
    esp_err_t err = transport_zigbee_radio_platform_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(coordinator_task, "fleet_zb", 16384, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "coordinator task started");
    return ESP_OK;
}

#else /* !CONFIG_FLEET_ZIGBEE_ENABLE */

esp_err_t transport_zigbee_radio_platform_init(void)
{
    return ESP_OK;
}

esp_err_t transport_zigbee_radio_start(void)
{
    return ESP_OK;
}

#endif

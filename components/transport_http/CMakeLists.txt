idf_component_register(SRCS "transport_http.c" "http_api.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash json
                             can_obd obd_poller profile_store
                             telemetry_bus sys_runtime obd_codec cmd_policy
                             net_lte telemetry_uplink esp_app_format fw_ota fw_ota_lte
                             freertos esp_event esp_timer)

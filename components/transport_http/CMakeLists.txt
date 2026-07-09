idf_component_register(SRCS "transport_http.c" "http_api.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash json
                             ble_elm elm327_client obd_poller profile_store
                             telemetry_bus sys_runtime obd_codec cmd_policy
                             freertos esp_event esp_timer)

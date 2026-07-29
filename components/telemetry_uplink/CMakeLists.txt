idf_component_register(SRCS "telemetry_uplink.c" "uplink_payload.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash freertos esp_timer
                             telemetry_bus sys_runtime ble_elm elm327_client
                             obd_poller profile_store net_lte)

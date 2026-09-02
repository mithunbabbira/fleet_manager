idf_component_register(SRCS "transport_zigbee.c" "transport_zigbee_radio.c"
                    INCLUDE_DIRS "include"
                    REQUIRES host_registry fleet_protocol sys_runtime nvs_flash esp_timer esp-zigbee-lib driver
                    PRIV_REQUIRES esp_wifi esp_coex)

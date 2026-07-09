idf_component_register(SRCS "ble_elm.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt nvs_flash freertos elm_transport profile_store telemetry_bus esp_event esp_timer)

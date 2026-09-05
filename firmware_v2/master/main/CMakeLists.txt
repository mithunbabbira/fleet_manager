idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES board can_obd obd_poller lte ota cli uplink transport_zigbee
                             store_sd nvs_flash app_update driver)

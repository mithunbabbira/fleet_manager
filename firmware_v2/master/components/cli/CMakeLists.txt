idf_component_register(SRCS "cli.c" "cli_format.c"
                    INCLUDE_DIRS "include"
                    REQUIRES lte ota uplink can_obd obd_poller host_registry store_sd
                             fleet_protocol esp_timer)

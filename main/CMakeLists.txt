idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash sys_runtime profile_store telemetry_bus
                             can_obd obd_poller transport_serial transport_http
                             net_lte telemetry_uplink fw_ota fw_ota_lte store_sd)

idf_component_register(SRCS "obd_poller.c"
                    INCLUDE_DIRS "include"
                    REQUIRES can_obd cmd_policy obd_codec profile_store telemetry_bus sys_runtime freertos esp_timer esp_common)

idf_component_register(SRCS "transport_serial.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ble_elm elm327_client obd_poller profile_store telemetry_bus sys_runtime cmd_policy freertos)

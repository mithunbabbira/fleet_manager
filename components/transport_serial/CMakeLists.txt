idf_component_register(SRCS "transport_serial.c"
                    INCLUDE_DIRS "include"
                    REQUIRES can_obd cmd_policy obd_poller profile_store telemetry_bus
                             sys_runtime net_lte telemetry_uplink freertos driver vfs)

idf_component_register(SRCS "transport_serial.c"
                    INCLUDE_DIRS "include"
                    REQUIRES can_obd cmd_policy obd_poller profile_store telemetry_bus
                             sys_runtime net_lte telemetry_uplink fw_ota fw_ota_lte store_sd
                             host_registry transport_zigbee
                             freertos driver vfs)

idf_component_register(SRCS "uplink.c" "uplink_envelope.c" "uplink_obd.c" "uplink_host.c"
                             "uplink_queue.c" "uplink_batch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES lte ota obd_poller host_registry store_sd nvs_flash esp_timer
                             freertos log)

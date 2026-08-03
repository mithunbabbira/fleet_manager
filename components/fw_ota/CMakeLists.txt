idf_component_register(SRCS "fw_ota.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update esp_partition mbedtls freertos log esp_app_format)

idf_component_register(SRCS "ota_flash.c" "ota_cloud.c" "ota_parse.c"
                    INCLUDE_DIRS "include"
                    REQUIRES lte nvs_flash json app_update esp_partition mbedtls freertos log esp_app_format)

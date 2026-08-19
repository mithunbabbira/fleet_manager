idf_component_register(SRCS "fw_ota_lte.c" "fw_ota_lte_parse.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fw_ota net_lte nvs_flash json freertos log esp_app_format)

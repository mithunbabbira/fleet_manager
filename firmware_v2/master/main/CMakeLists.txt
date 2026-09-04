idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES board lte ota cli nvs_flash app_update)

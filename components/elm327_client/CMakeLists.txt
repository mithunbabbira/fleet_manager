idf_component_register(SRCS "elm327_client.c"
                    INCLUDE_DIRS "include"
                    REQUIRES elm_transport freertos esp_common)

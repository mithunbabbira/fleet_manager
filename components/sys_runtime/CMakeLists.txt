idf_component_register(SRCS "sys_runtime.c"
                    INCLUDE_DIRS "include"
                    REQUIRES freertos esp_timer esp_system log)

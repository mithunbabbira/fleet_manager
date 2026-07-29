idf_component_register(SRCS "can_obd.c" "mcp2515.c" "obd_isotp.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_timer nvs_flash)

idf_component_register(SRCS "obd_poller.c"
                       INCLUDE_DIRS "include"
                       REQUIRES can_obd obd_codec
                       PRIV_REQUIRES esp_timer freertos log)

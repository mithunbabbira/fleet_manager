set(ZB_REQUIRES host_registry fleet_protocol nvs_flash esp_timer driver)
# Zigbee stack only when radio enabled (see Kconfig FLEET_ZIGBEE_ENABLE).
# Component manager deps in idf_component.yml are always declared so idf.py
# can fetch them; link only when the coordinator path is compiled in.
if(CONFIG_FLEET_ZIGBEE_ENABLE)
    list(APPEND ZB_REQUIRES esp-zigbee-lib)
endif()

idf_component_register(SRCS "transport_zigbee.c" "transport_zigbee_radio.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES ${ZB_REQUIRES}
                    PRIV_REQUIRES esp_wifi esp_coex)

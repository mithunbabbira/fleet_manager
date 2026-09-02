set(TRANSPORT_HTTP_SRCS "transport_http.c")

if(CONFIG_ELM_HTTP_ENABLE)
    list(APPEND TRANSPORT_HTTP_SRCS "http_api.c")
endif()

set(TRANSPORT_HTTP_REQUIRES
    nvs_flash can_obd obd_poller profile_store
    telemetry_bus sys_runtime obd_codec cmd_policy
    net_lte telemetry_uplink esp_app_format fw_ota fw_ota_lte
    host_registry transport_zigbee
    freertos esp_event esp_timer)

if(CONFIG_ELM_HTTP_ENABLE)
    list(APPEND TRANSPORT_HTTP_REQUIRES esp_http_server esp_wifi esp_netif json)
endif()

idf_component_register(SRCS ${TRANSPORT_HTTP_SRCS}
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES ${TRANSPORT_HTTP_REQUIRES})

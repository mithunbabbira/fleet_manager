cmake_minimum_required(VERSION 3.16)
project(elm_host_tests C)
set(CMAKE_C_STANDARD 11)

add_executable(test_cmd_policy
    test_cmd_policy.c
    ${CMAKE_SOURCE_DIR}/../../components/cmd_policy/cmd_policy.c
)
target_include_directories(test_cmd_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/cmd_policy/include
)

add_executable(test_obd_codec
    test_obd_codec.c
    ${CMAKE_SOURCE_DIR}/../../components/obd_codec/obd_codec.c
)
target_include_directories(test_obd_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/obd_codec/include
)
target_link_libraries(test_obd_codec m)

add_executable(test_uplink_payload
    test_uplink_payload.c
    ${CMAKE_SOURCE_DIR}/../../components/telemetry_uplink/uplink_payload.c
)
target_include_directories(test_uplink_payload PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/telemetry_uplink/include
)
target_link_libraries(test_uplink_payload m)

add_executable(test_obd_isotp
    test_obd_isotp.c
    ${CMAKE_SOURCE_DIR}/../../components/can_obd/obd_isotp.c
)
target_include_directories(test_obd_isotp PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/can_obd/include
)

add_executable(test_fw_ota_lte_parse
    test_fw_ota_lte_parse.c
    ${CMAKE_SOURCE_DIR}/../../components/fw_ota_lte/fw_ota_lte_parse.c
    $ENV{IDF_PATH}/components/json/cJSON/cJSON.c
)
target_include_directories(test_fw_ota_lte_parse PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/fw_ota_lte/include
    $ENV{IDF_PATH}/components/json/cJSON
)

enable_testing()
add_test(NAME test_cmd_policy COMMAND test_cmd_policy)
add_test(NAME test_obd_codec COMMAND test_obd_codec)
add_test(NAME test_uplink_payload COMMAND test_uplink_payload)
add_test(NAME test_obd_isotp COMMAND test_obd_isotp)
add_test(NAME test_fw_ota_lte_parse COMMAND test_fw_ota_lte_parse)

add_executable(test_fleet_tlv
    test_fleet_tlv.c
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/fleet_tlv.c
)
target_include_directories(test_fleet_tlv PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/include
)

add_executable(test_host_registry
    test_host_registry.c
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/fleet_tlv.c
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/host_registry.c
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/fleet_manifest_catalog.c
)
target_include_directories(test_host_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/shims
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/include
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/include
)
target_compile_definitions(test_host_registry PRIVATE HOST_REGISTRY_HOST_TEST=1)

add_test(NAME test_fleet_tlv COMMAND test_fleet_tlv)
add_test(NAME test_host_registry COMMAND test_host_registry)

add_executable(test_fleet_uplink_path
    test_fleet_uplink_path.c
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/fleet_tlv.c
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/host_registry.c
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/fleet_manifest_catalog.c
    ${CMAKE_SOURCE_DIR}/../../components/telemetry_uplink/uplink_payload.c
)
target_include_directories(test_fleet_uplink_path PRIVATE
    ${CMAKE_SOURCE_DIR}/shims
    ${CMAKE_SOURCE_DIR}/../../components/fleet_protocol/include
    ${CMAKE_SOURCE_DIR}/../../components/host_registry/include
    ${CMAKE_SOURCE_DIR}/../../components/telemetry_uplink/include
)
target_compile_definitions(test_fleet_uplink_path PRIVATE HOST_REGISTRY_HOST_TEST=1)
target_link_libraries(test_fleet_uplink_path m)

add_test(NAME test_fleet_uplink_path COMMAND test_fleet_uplink_path)

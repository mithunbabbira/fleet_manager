cmake_minimum_required(VERSION 3.16)
project(elm_host_tests C)
set(CMAKE_C_STANDARD 11)

set(V2_MASTER "${CMAKE_SOURCE_DIR}/../../firmware_v2/master")

enable_testing()

configure_file(
    ${CMAKE_SOURCE_DIR}/shims/esp_shim.h
    ${CMAKE_CURRENT_BINARY_DIR}/shims/esp_err.h
    COPYONLY
)

add_executable(test_cli_format_v2
    test_cli_format_v2.c
    ${V2_MASTER}/components/cli/cli_format.c
    ${V2_MASTER}/components/lte/lte_time.c
)
target_include_directories(test_cli_format_v2 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/shims
    ${V2_MASTER}/components/cli/include
    ${V2_MASTER}/components/lte/include
)
add_test(NAME test_cli_format_v2 COMMAND test_cli_format_v2)

add_executable(test_uplink_envelope_v2
    test_uplink_envelope_v2.c
    ${V2_MASTER}/components/uplink/uplink_envelope.c
)
target_include_directories(test_uplink_envelope_v2 PRIVATE
    ${V2_MASTER}/components/uplink/include
)
target_link_libraries(test_uplink_envelope_v2 m)
add_test(NAME test_uplink_envelope_v2 COMMAND test_uplink_envelope_v2)

add_executable(test_uplink_obd_v2
    test_uplink_obd_v2.c
    ${V2_MASTER}/components/uplink/uplink_obd.c
    ${V2_MASTER}/components/uplink/uplink_envelope.c
)
target_include_directories(test_uplink_obd_v2 PRIVATE
    ${V2_MASTER}/components/uplink/include
)
target_link_libraries(test_uplink_obd_v2 m)
add_test(NAME test_uplink_obd_v2 COMMAND test_uplink_obd_v2)

add_executable(test_uplink_host_v2
    test_uplink_host_v2.c
    ${V2_MASTER}/components/uplink/uplink_host.c
)
target_include_directories(test_uplink_host_v2 PRIVATE
    ${V2_MASTER}/components/uplink/include
)
target_link_libraries(test_uplink_host_v2 m)
add_test(NAME test_uplink_host_v2 COMMAND test_uplink_host_v2)

add_executable(test_uplink_queue_v2
    test_uplink_queue_v2.c
    ${V2_MASTER}/components/uplink/uplink_queue.c
)
target_include_directories(test_uplink_queue_v2 PRIVATE
    ${V2_MASTER}/components/uplink/include
)
add_test(NAME test_uplink_queue_v2 COMMAND test_uplink_queue_v2)

add_executable(test_uplink_batch_v2
    test_uplink_batch_v2.c
    ${V2_MASTER}/components/uplink/uplink_batch.c
)
target_include_directories(test_uplink_batch_v2 PRIVATE
    ${V2_MASTER}/components/uplink/include
)
add_test(NAME test_uplink_batch_v2 COMMAND test_uplink_batch_v2)

add_executable(test_fleet_zb_epan
    test_fleet_zb_epan.c
    ${V2_MASTER}/components/transport_zigbee/fleet_zb_epan.c
)
target_include_directories(test_fleet_zb_epan PRIVATE
    ${V2_MASTER}/components/transport_zigbee/include
)
add_test(NAME test_fleet_zb_epan COMMAND test_fleet_zb_epan)

# ota_parse.c has no ESP-IDF dependency (plain C + cJSON), but does need a
# real cJSON to link against. Discovered portably so this runs both in CI
# (apt install libcjson-dev on Ubuntu) and locally (e.g. brew install cjson
# on macOS) without hardcoding an include/lib path.
find_path(CJSON_INCLUDE_DIR NAMES cJSON.h PATH_SUFFIXES cjson)
find_library(CJSON_LIBRARY NAMES cjson)

if(CJSON_INCLUDE_DIR AND CJSON_LIBRARY)
    add_executable(test_ota_parse_v2
        test_ota_parse_v2.c
        ${V2_MASTER}/components/ota/ota_parse.c
    )
    target_include_directories(test_ota_parse_v2 PRIVATE
        ${CJSON_INCLUDE_DIR}
        ${V2_MASTER}/components/ota/include
    )
    target_link_libraries(test_ota_parse_v2 ${CJSON_LIBRARY})
    add_test(NAME test_ota_parse_v2 COMMAND test_ota_parse_v2)
else()
    message(WARNING "cJSON not found — skipping test_ota_parse_v2. "
                    "Install libcjson-dev (Ubuntu) or 'brew install cjson' (macOS).")
endif()

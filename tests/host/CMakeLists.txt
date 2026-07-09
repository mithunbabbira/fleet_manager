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

enable_testing()
add_test(NAME test_cmd_policy COMMAND test_cmd_policy)
add_test(NAME test_obd_codec COMMAND test_obd_codec)

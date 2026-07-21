cmake_minimum_required(VERSION 3.16)
project(synthetic_data_tests C)

set(CMAKE_C_STANDARD 11)
add_executable(test_synthetic_data
    test_synthetic_data.c
    ../main/synthetic_data.c
)
target_include_directories(test_synthetic_data PRIVATE ../main)
target_link_libraries(test_synthetic_data m)

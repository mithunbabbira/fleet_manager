cmake_minimum_required(VERSION 3.16)

# Keep in sync with tools/ota_dev_server publish folder name / backend manifest.version
file(READ "${CMAKE_SOURCE_DIR}/VERSION" PROJECT_VER_RAW)
string(STRIP "${PROJECT_VER_RAW}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elm327_esp32c6)

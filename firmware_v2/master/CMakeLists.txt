cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" PROJECT_VER_RAW)
string(STRIP "${PROJECT_VER_RAW}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fleet_v2_master)

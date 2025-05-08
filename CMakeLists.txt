cmake_minimum_required(VERSION 3.16)
set(PROJ_PARTITION_CSV "${CMAKE_SOURCE_DIR}/partitions.csv")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_scan)
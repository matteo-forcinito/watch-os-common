cmake_minimum_required(VERSION 3.16)
get_filename_component(WATCH_OS_COMMON "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(DEFINED ENV{WATCH_OS_COMMON_DIR})
    file(TO_CMAKE_PATH "$ENV{WATCH_OS_COMMON_DIR}" WATCH_OS_COMMON)
endif()
if(NOT DEFINED ENV{LITE_BOARD_DIR})
    message(FATAL_ERROR "Set LITE_BOARD_DIR to a supported firmware project")
endif()
file(TO_CMAKE_PATH "$ENV{LITE_BOARD_DIR}" LITE_BOARD_DIR)
set(EXTRA_COMPONENT_DIRS "${WATCH_OS_COMMON}/lite/main" "${LITE_BOARD_DIR}/components/hardware"
    "${WATCH_OS_COMMON}/components/board_api" "${WATCH_OS_COMMON}/components/board_drivers"
    "${WATCH_OS_COMMON}/components/extapp_sdk")
# Explicit dependency closure: no core/UI/apps component, and no LVGL.
set(COMPONENTS main)
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(watch_lite)

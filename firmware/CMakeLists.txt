# Top-level ESP-IDF / ESP-Matter project for the Matter Thread Thermostat.
#
# Prerequisites (exported in your shell before building):
#   - ESP-IDF v5.2+            : source $IDF_PATH/export.sh
#   - ESP-Matter               : source $ESP_MATTER_PATH/export.sh
#
# Build:
#   idf.py set-target esp32c6
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

# Pull in ESP-Matter and the connectedhomeip components, plus our local components.
# ESP_MATTER_PATH is set by esp-matter's export.sh.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "$ENV{ESP_MATTER_PATH}/components"
    "$ENV{ESP_MATTER_PATH}/examples/common"
    "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/config/esp32/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_thermostat)

# ESP-Matter / CHIP build flags (mirrors the esp-matter examples).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS   "-Os" APPEND)

# Top-level ESP-IDF / ESP-Matter project for the Matter Thread Thermostat.
#
# Prerequisites (exported in your shell before building):
#   - ESP-IDF v5.2+            : source $IDF_PATH/export.sh
#
# Matter core is pulled from the pre-built managed component espressif/esp_matter
# (see main/idf_component.yml) — the same approach as the sibling rgb-light/button/
# hub projects — so no connectedhomeip source build (and no gn/pigweed) is needed.
#
# Build:
#   idf.py set-target esp32c6
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

# Local components only; Matter arrives via the managed component and any downloaded
# managed_components/ dir.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_thermostat)

# ESP-Matter / CHIP build flags (mirrors the esp-matter examples).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS   "-Os" APPEND)

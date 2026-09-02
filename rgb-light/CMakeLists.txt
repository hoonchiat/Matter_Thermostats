# Matter RGB light bulb (ESP32-C6, Thread).
# Matter core comes from the managed component espressif/esp_matter (same as the hub),
# so no bootstrapped connectedhomeip is needed. The ws2812 LED + iot_button drivers and
# the app_reset helper come from the esp-matter repo's device_hal + examples/common;
# those are plain drivers/helpers with no connectedhomeip dependency.
cmake_minimum_required(VERSION 3.16.0)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
endif()

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Set ESP_MATTER_PATH to the esp-matter repo (used for device_hal + app_reset)")
endif()
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32c6_devkit_c)

# Sets led_type=ws2812, button_type=iot and extra_components_dirs_append (led_driver + iot_button).
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common/app_reset"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(light)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)

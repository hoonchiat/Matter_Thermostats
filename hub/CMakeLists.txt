cmake_minimum_required(VERSION 3.16.0)

# If managed_components were downloaded by the IDF Component Manager, add them.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
endif()

# Fallback: manually cloned esp-matter at a short path (e.g. C:\em).
# Set $Env:ESP_MATTER_PATH before building to use this path instead.
if(DEFINED ENV{ESP_MATTER_PATH})
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{ESP_MATTER_PATH}/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_hub)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-D_NO_GENERIC_THREAD_NETWORK_COMMISSIONING_DRIVER_" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)
# PBUF_POOL_SIZE is irrelevant: MEMP_MEM_MALLOC=1 makes the pool heap-backed with no limit.

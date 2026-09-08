idf_component_register(
    SRCS
        "app_main.cpp"
        "app_matter.cpp"
        "app_control.cpp"
        "app_nvs.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        # Local drivers / logic components:
        thermistor
        rotary_encoder
        button
        thermostat_core
        relays
        ui_oled
        occupancy
        # ESP-IDF + Matter:
        nvs_flash
        esp_matter
        esp_matter_console
)

# ESP-Matter data-model codegen sometimes needs the CHIP config on the include path.
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

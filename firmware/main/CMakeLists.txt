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
        sht4x
        rotary_encoder
        button
        thermostat_core
        relays
        ui_oled
        occupancy
        i18n
        # ESP-IDF + Matter:
        nvs_flash
        esp_matter
        # esp_matter_console is only needed when CONFIG_ENABLE_CHIP_SHELL=y
        # (diagnostics CLI); it is off by default, so the console calls in
        # app_matter.cpp compile out and the component isn't required.
)

# ESP-Matter data-model codegen sometimes needs the CHIP config on the include path.
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

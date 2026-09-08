idf_component_register(
    SRCS
        "thermistor.c"
        "ntc_curves.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_adc
)

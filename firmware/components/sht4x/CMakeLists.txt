idf_component_register(
    SRCS
        "sht4x.c"
        "sht4x_logic.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c
)

idf_component_register(
    SRCS
        "occupancy.c"
        "occupancy_logic.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio
)

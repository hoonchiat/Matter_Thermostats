idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_driver_gpio
)

idf_component_register(
    SRCS "ui_oled.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c esp_lcd
    # The SSD1306 panel driver arrives as a managed component (see main/idf_component.yml):
    #   espressif/esp_lcd_ssd1306
)

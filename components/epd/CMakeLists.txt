idf_component_register(
    SRCS "epd.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_spi
)

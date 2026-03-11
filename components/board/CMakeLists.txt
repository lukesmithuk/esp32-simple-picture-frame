idf_component_register(
    SRCS
        "board.c"
        "axp2101.cpp"
        "pcf85063.c"
        "XPowersLib/XPowersLibInterface.cpp"
    INCLUDE_DIRS
        "include"
        "."
        "XPowersLib"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_hw_support
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    XPOWERS_CHIP_AXP2101
    CONFIG_XPOWERS_ESP_IDF_NEW_API
)

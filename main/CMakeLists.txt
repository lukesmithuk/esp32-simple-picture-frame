idf_component_register(SRCS "main.c"
                             "tests.c"
                        INCLUDE_DIRS "."
                        REQUIRES pmic
                                 epd
                                 esp_driver_i2c
                                 esp_driver_gpio
                                 esp_driver_sdmmc
                                 sdmmc
                                 fatfs
                                 esp_hw_support
                                 heap)

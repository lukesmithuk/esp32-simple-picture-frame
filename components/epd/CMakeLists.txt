idf_component_register(SRCS "epd.c"
                             "epd_tests.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_driver_spi
                                 esp_driver_gpio
                                 log
                                 freertos
                                 heap)

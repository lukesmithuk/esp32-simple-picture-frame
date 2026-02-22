idf_component_register(SRCS "pmic.c"
                             "pmic_tests.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_driver_i2c
                                 log
                                 freertos)

idf_component_register(
    SRCS "image_decode.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_hw_support
        epd
)

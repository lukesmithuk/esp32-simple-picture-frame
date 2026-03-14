set(srcs "main.c")
if(CONFIG_TEST_MODE)
    list(APPEND srcs "test_main.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        board
        epd
        sdcard
        image_picker
        image_loader
        image_decode
        epd_text
        errlog
)

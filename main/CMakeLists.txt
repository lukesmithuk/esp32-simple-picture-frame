idf_component_register(
    SRCS "main.c" "test_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        epd
)

/*
 * Unity component tests for the epd component.
 *
 * Run with: idf.py -T components/epd/test
 *
 * Hardware integration tests (solid colour, checkerboard) live in
 * main/test_main.c (TEST_MODE).
 */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "epd.h"

TEST_CASE("epd_fill_color packs nibbles correctly", "[epd]")
{
    uint8_t buf[EPD_BUF_SIZE];
    epd_fill_color(buf, EPD_COLOR_RED);
    /* EPD_COLOR_RED = 4; packed byte = 0x44 */
    TEST_ASSERT_EACH_EQUAL_UINT8(0x44, buf, EPD_BUF_SIZE);
}

TEST_CASE("epd_fill_color white", "[epd]")
{
    uint8_t buf[EPD_BUF_SIZE];
    epd_fill_color(buf, EPD_COLOR_WHITE);
    /* EPD_COLOR_WHITE = 1; packed byte = 0x11 */
    TEST_ASSERT_EACH_EQUAL_UINT8(0x11, buf, EPD_BUF_SIZE);
}

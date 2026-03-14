#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WIDTH   800
#define EPD_HEIGHT  480
#define EPD_BUF_SIZE (EPD_WIDTH / 2 * EPD_HEIGHT)   /* 192 000 bytes, 4bpp packed */

/**
 * Palette colour indices for Spectra 6 (E6) panel.
 * High nibble = even pixel, low nibble = odd pixel.
 */
typedef enum {
    EPD_COLOR_BLACK  = 0,
    EPD_COLOR_WHITE  = 1,
    EPD_COLOR_YELLOW = 2,
    EPD_COLOR_RED    = 3,
    /* 4 = clean/reserved — not a displayable colour */
    EPD_COLOR_BLUE   = 5,
    EPD_COLOR_GREEN  = 6,
} epd_color_t;

/**
 * @brief Initialise SPI bus and device.  Does NOT contact the panel.
 *
 * Requires board_epd_power(true) to have been called first.
 */
esp_err_t epd_init(void);

/**
 * @brief Display a 4bpp packed frame buffer.
 *
 * Performs a full panel cycle on every call:
 *   hw_reset → init_sequence → 0x10 + data → wait_busy
 *   → 0x04 PON → wait_busy → 0x12{00} DRF → wait_busy
 *   → 0x02{00} POF → wait_busy → 0x07{A5} DSLP
 *
 * @param frame_buf  Pointer to EPD_BUF_SIZE bytes.  Must be allocated with
 *                   epd_alloc_frame_buf() (PSRAM-backed) or from PSRAM
 *                   directly to avoid DMA constraints.
 */
esp_err_t epd_display(const uint8_t *frame_buf);

/** @brief Release SPI bus and device. */
void epd_deinit(void);

/**
 * @brief Allocate a frame buffer from PSRAM.
 * @return Pointer to EPD_BUF_SIZE bytes, or NULL on failure.
 *         Caller must free() when done.
 */
uint8_t *epd_alloc_frame_buf(void);

/**
 * @brief Fill a frame buffer with a solid colour.
 * @param frame_buf  Must be EPD_BUF_SIZE bytes.
 * @param color      One of the EPD_COLOR_* constants.
 */
void epd_fill_color(uint8_t *frame_buf, epd_color_t color);

#ifdef __cplusplus
}
#endif

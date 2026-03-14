#pragma once

#include <stdint.h>
#include "epd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render an ASCII string into a 4bpp EPD frame buffer.
 *
 * Uses an 8x8 bitmap font. Characters outside printable ASCII [0x20–0x7E]
 * are rendered as a solid block. Pixels outside the buffer are clipped.
 *
 * @param frame_buf  EPD_BUF_SIZE-byte buffer (4bpp packed, 800x480).
 * @param x          Starting column in pixels (0 = left).
 * @param y          Starting row in pixels (0 = top).
 * @param text       NUL-terminated string.
 * @param fg         Foreground palette index (e.g. EPD_COLOR_BLACK).
 * @param bg         Background palette index (e.g. EPD_COLOR_WHITE).
 * @param scale      Magnification factor (1 = native 8x8, 2 = 16x16, etc.).
 */
void epd_text_draw(uint8_t *frame_buf, int x, int y,
                   const char *text, epd_color_t fg, epd_color_t bg,
                   int scale);

/**
 * @brief Draw text centred horizontally on the display.
 *
 * Does not clear the frame buffer — caller should fill first if needed.
 *
 * @param frame_buf  EPD_BUF_SIZE-byte buffer.
 * @param y          Row in pixels.
 * @param text       NUL-terminated string.
 * @param fg         Foreground colour.
 * @param bg         Background colour (used for glyph background pixels).
 * @param scale      Magnification factor.
 */
void epd_text_draw_centred(uint8_t *frame_buf, int y,
                           const char *text, epd_color_t fg, epd_color_t bg,
                           int scale);

/** @brief Returns pixel width of a string at the given scale. */
int epd_text_string_width(const char *text, int scale);

#ifdef __cplusplus
}
#endif

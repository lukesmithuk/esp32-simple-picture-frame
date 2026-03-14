#include <string.h>
#include "epd_text.h"
#include "font_8x8.h"

#define GLYPH_W 8
#define GLYPH_H 8

static inline void set_pixel(uint8_t *frame_buf, int px, int py, uint8_t color)
{
    if (px < 0 || px >= EPD_WIDTH || py < 0 || py >= EPD_HEIGHT) {
        return;
    }
    int idx = py * (EPD_WIDTH / 2) + px / 2;
    if (px % 2 == 0) {
        frame_buf[idx] = (frame_buf[idx] & 0x0F) | (color << 4);
    } else {
        frame_buf[idx] = (frame_buf[idx] & 0xF0) | (color & 0x0F);
    }
}

static void draw_glyph(uint8_t *frame_buf, int x, int y,
                        const uint8_t glyph[8],
                        uint8_t fg, uint8_t bg, int scale)
{
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++) {
            uint8_t color = (bits & (0x80 >> col)) ? fg : bg;
            /* Scale the pixel */
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    set_pixel(frame_buf,
                              x + col * scale + sx,
                              y + row * scale + sy,
                              color);
                }
            }
        }
    }
}

void epd_text_draw(uint8_t *frame_buf, int x, int y,
                   const char *text, epd_color_t fg, epd_color_t bg,
                   int scale)
{
    if (scale < 1) scale = 1;

    int cursor_x = x;
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        const uint8_t *glyph;
        if (ch >= 0x20 && ch <= 0x7E) {
            glyph = font_8x8[ch - 0x20];
        } else {
            /* Non-printable: solid block */
            static const uint8_t block[8] = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
            };
            glyph = block;
        }
        draw_glyph(frame_buf, cursor_x, y, glyph,
                    (uint8_t)fg, (uint8_t)bg, scale);
        cursor_x += GLYPH_W * scale;
    }
}

void epd_text_draw_centred(uint8_t *frame_buf, int y,
                           const char *text, epd_color_t fg, epd_color_t bg,
                           int scale)
{
    int w = epd_text_string_width(text, scale);
    int x = (EPD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    epd_text_draw(frame_buf, x, y, text, fg, bg, scale);
}

int epd_text_string_width(const char *text, int scale)
{
    if (scale < 1) scale = 1;
    return (int)strlen(text) * GLYPH_W * scale;
}

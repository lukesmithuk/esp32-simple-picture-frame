#include "image_decode.h"

#include <string.h>

#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

#define PALETTE_COUNT       6
#define YIELD_EVERY_N_ROWS  48  /* yield every N rows to let other tasks run */

static const char *TAG = "image_decode";

/* Measured palette RGB values for the Spectra 6 e-paper panel.
 * Indices match epd_color_t (0–5).  Source: aitjcize/esp32-photoframe
 * calibration, Waveshare 7.3" ACeP panel.  */
static const uint8_t PALETTE_RGB[PALETTE_COUNT][3] = {
    {  2,   2,   2},   /* EPD_COLOR_BLACK  = 0 */
    {190, 200, 200},   /* EPD_COLOR_WHITE  = 1 */
    { 39, 102,  60},   /* EPD_COLOR_GREEN  = 2 */
    {  5,  64, 158},   /* EPD_COLOR_BLUE   = 3 */
    {135,  19,   0},   /* EPD_COLOR_RED    = 4 */
    {205, 202,   0},   /* EPD_COLOR_YELLOW = 5 */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline void pack_pixel(uint8_t *fb, int x, int y, uint8_t color)
{
    int idx = y * (EPD_WIDTH / 2) + x / 2;
    if (x % 2 == 0)
        fb[idx] = (fb[idx] & 0x0F) | (color << 4);
    else
        fb[idx] = (fb[idx] & 0xF0) | (color & 0x0F);
}

static inline int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── JPEG decode ─────────────────────────────────────────────────────────── */

static esp_jpeg_image_scale_t select_scale(uint16_t w, uint16_t h)
{
    if (w >= 3200 || h >= 2400)
        return JPEG_IMAGE_SCALE_1_4;
    if (w >= 1600 || h >= 1200)
        return JPEG_IMAGE_SCALE_1_2;
    return JPEG_IMAGE_SCALE_0;
}

/**
 * Decode JPEG to an RGB888 PSRAM buffer.
 * Caller must free *out_rgb on success.
 */
static esp_err_t decode_jpeg(const uint8_t *jpeg_buf, size_t jpeg_size,
                             uint8_t **out_rgb, int *out_w, int *out_h)
{
    /* Query dimensions without decoding. */
    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpeg_buf,
        .indata_size = jpeg_size,
        .outbuf      = NULL,
        .outbuf_size = 0,
        .out_format  = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;

    esp_err_t ret = esp_jpeg_get_image_info(&cfg, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_get_image_info failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    uint16_t src_w = info.width;
    uint16_t src_h = info.height;
    ESP_LOGI(TAG, "JPEG source: %ux%u", src_w, src_h);

    /* Pick a decode scale to keep the intermediate buffer manageable. */
    esp_jpeg_image_scale_t scale = select_scale(src_w, src_h);
    if (scale != JPEG_IMAGE_SCALE_0) {
        /* Re-query to get post-scale dimensions. */
        esp_jpeg_image_cfg_t scaled_cfg = {
            .indata      = (uint8_t *)jpeg_buf,
            .indata_size = jpeg_size,
            .out_format  = JPEG_IMAGE_FORMAT_RGB888,
            .out_scale   = scale,
        };
        ret = esp_jpeg_get_image_info(&scaled_cfg, &info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_jpeg_get_image_info (scaled) failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Decode scale 1/%d → %ux%u",
                 1 << scale, info.width, info.height);
    }

    /* Use output_len from the library — accounts for MCU padding. */
    size_t rgb_size = info.output_len;
    uint8_t *rgb = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "Failed to alloc decode buffer (%zu bytes)", rgb_size);
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t decode_cfg = {
        .indata      = (uint8_t *)jpeg_buf,
        .indata_size = jpeg_size,
        .outbuf      = rgb,
        .outbuf_size = rgb_size,
        .out_format  = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale   = scale,
    };

    ret = esp_jpeg_decode(&decode_cfg, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_decode failed: %s", esp_err_to_name(ret));
        free(rgb);
        return ESP_FAIL;
    }

    *out_rgb = rgb;
    *out_w   = info.width;
    *out_h   = info.height;
    ESP_LOGI(TAG, "Decoded to %dx%d RGB888 (%zu bytes)", *out_w, *out_h, rgb_size);
    return ESP_OK;
}

/* ── Scale (cover + centre-crop, nearest-neighbor) ───────────────────────── */

/**
 * Scale rgb_raw (raw_w × raw_h) to EPD_WIDTH × EPD_HEIGHT using cover mode.
 * Allocates a new PSRAM buffer for the result and frees rgb_raw.
 */
static esp_err_t scale_to_display(uint8_t *rgb_raw, int raw_w, int raw_h,
                                  uint8_t **out_rgb)
{
    size_t out_size = (size_t)EPD_WIDTH * EPD_HEIGHT * 3;
    uint8_t *scaled = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
    if (!scaled) {
        ESP_LOGE(TAG, "Failed to alloc scale buffer (%zu bytes)", out_size);
        free(rgb_raw);
        return ESP_ERR_NO_MEM;
    }

    /* Cover mode: scale so the image fills the display, then centre-crop. */
    int scale_x_fp = (raw_w << 10) / EPD_WIDTH;   /* Q10 fixed-point */
    int scale_y_fp = (raw_h << 10) / EPD_HEIGHT;
    int scale_fp   = scale_x_fp < scale_y_fp ? scale_x_fp : scale_y_fp;

    /* Source region that maps to the display. */
    int crop_w = (EPD_WIDTH  * scale_fp) >> 10;
    int crop_h = (EPD_HEIGHT * scale_fp) >> 10;
    int src_x0 = (raw_w - crop_w) / 2;
    int src_y0 = (raw_h - crop_h) / 2;

    ESP_LOGI(TAG, "Scale %dx%d → %dx%d (crop origin %d,%d, crop %dx%d)",
             raw_w, raw_h, EPD_WIDTH, EPD_HEIGHT, src_x0, src_y0, crop_w, crop_h);

    for (int dy = 0; dy < EPD_HEIGHT; dy++) {
        for (int dx = 0; dx < EPD_WIDTH; dx++) {
            int sx = src_x0 + (dx * crop_w) / EPD_WIDTH;
            int sy = src_y0 + (dy * crop_h) / EPD_HEIGHT;
            sx = clamp(sx, 0, raw_w - 1);
            sy = clamp(sy, 0, raw_h - 1);

            int dst_idx = (dy * EPD_WIDTH + dx) * 3;
            int src_idx = (sy * raw_w + sx) * 3;
            scaled[dst_idx]     = rgb_raw[src_idx];
            scaled[dst_idx + 1] = rgb_raw[src_idx + 1];
            scaled[dst_idx + 2] = rgb_raw[src_idx + 2];
        }
        if (dy % YIELD_EVERY_N_ROWS == 0)
            vTaskDelay(1);
    }

    free(rgb_raw);
    *out_rgb = scaled;
    return ESP_OK;
}

/* ── Floyd-Steinberg dither → 4bpp frame buffer ─────────────────────────── */

static int find_closest_color(int r, int g, int b)
{
    int best = 0;
    int best_dist = INT32_MAX;

    for (int i = 0; i < PALETTE_COUNT; i++) {
        int dr = r - PALETTE_RGB[i][0];
        int dg = g - PALETTE_RGB[i][1];
        int db = b - PALETTE_RGB[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

/**
 * Floyd-Steinberg dither the scaled RGB888 buffer into the 4bpp frame buffer.
 * Frees rgb_scaled when done (even on failure).
 */
static esp_err_t dither_to_framebuf(uint8_t *rgb_scaled, uint8_t *frame_buf)
{
    /* Two rows of error accumulators (R, G, B per pixel).
     * Using int16_t keeps the buffer in internal RAM (~10 KB). */
    int16_t *err_cur  = calloc(EPD_WIDTH * 3, sizeof(int16_t));
    int16_t *err_next = calloc(EPD_WIDTH * 3, sizeof(int16_t));

    if (!err_cur || !err_next) {
        ESP_LOGE(TAG, "Failed to alloc error buffers");
        free(err_cur);
        free(err_next);
        free(rgb_scaled);
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < EPD_HEIGHT; y++) {
        for (int x = 0; x < EPD_WIDTH; x++) {
            int img_idx = (y * EPD_WIDTH + x) * 3;
            int err_idx = x * 3;

            int r = clamp(rgb_scaled[img_idx]     + err_cur[err_idx],     0, 255);
            int g = clamp(rgb_scaled[img_idx + 1] + err_cur[err_idx + 1], 0, 255);
            int b = clamp(rgb_scaled[img_idx + 2] + err_cur[err_idx + 2], 0, 255);

            int ci = find_closest_color(r, g, b);
            pack_pixel(frame_buf, x, y, (uint8_t)ci);

            int er = r - PALETTE_RGB[ci][0];
            int eg = g - PALETTE_RGB[ci][1];
            int eb = b - PALETTE_RGB[ci][2];

            /* Distribute error:  7/16 right, 3/16 below-left,
             *                    5/16 below, 1/16 below-right  */
            if (x + 1 < EPD_WIDTH) {
                err_cur[(x + 1) * 3]     += (er * 7) / 16;
                err_cur[(x + 1) * 3 + 1] += (eg * 7) / 16;
                err_cur[(x + 1) * 3 + 2] += (eb * 7) / 16;
            }
            if (y + 1 < EPD_HEIGHT) {
                if (x > 0) {
                    err_next[(x - 1) * 3]     += (er * 3) / 16;
                    err_next[(x - 1) * 3 + 1] += (eg * 3) / 16;
                    err_next[(x - 1) * 3 + 2] += (eb * 3) / 16;
                }
                err_next[x * 3]     += (er * 5) / 16;
                err_next[x * 3 + 1] += (eg * 5) / 16;
                err_next[x * 3 + 2] += (eb * 5) / 16;
                if (x + 1 < EPD_WIDTH) {
                    err_next[(x + 1) * 3]     += (er * 1) / 16;
                    err_next[(x + 1) * 3 + 1] += (eg * 1) / 16;
                    err_next[(x + 1) * 3 + 2] += (eb * 1) / 16;
                }
            }
        }

        /* Rotate error rows. */
        int16_t *tmp = err_cur;
        err_cur  = err_next;
        err_next = tmp;
        memset(err_next, 0, EPD_WIDTH * 3 * sizeof(int16_t));

        if (y % YIELD_EVERY_N_ROWS == 0)
            vTaskDelay(1);
    }

    free(err_cur);
    free(err_next);
    free(rgb_scaled);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t image_decode_jpeg(const uint8_t *jpeg_buf, size_t jpeg_size,
                            uint8_t *frame_buf)
{
    if (!jpeg_buf || jpeg_size == 0 || !frame_buf)
        return ESP_ERR_INVALID_ARG;

    /* Step 1: Decode JPEG → RGB888 */
    uint8_t *rgb_raw = NULL;
    int decode_w = 0, decode_h = 0;
    esp_err_t ret = decode_jpeg(jpeg_buf, jpeg_size, &rgb_raw, &decode_w, &decode_h);
    if (ret != ESP_OK)
        return ret;

    /* Step 2: Scale to display (cover + centre-crop).
     * scale_to_display frees rgb_raw. */
    uint8_t *rgb_scaled = NULL;
    if (decode_w == EPD_WIDTH && decode_h == EPD_HEIGHT) {
        /* Already the right size — skip scaling. */
        rgb_scaled = rgb_raw;
    } else {
        ret = scale_to_display(rgb_raw, decode_w, decode_h, &rgb_scaled);
        if (ret != ESP_OK)
            return ret;
    }

    /* Step 3: Floyd-Steinberg dither → 4bpp frame buffer.
     * dither_to_framebuf frees rgb_scaled. */
    ret = dither_to_framebuf(rgb_scaled, frame_buf);
    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "Decode pipeline complete");
    return ESP_OK;
}

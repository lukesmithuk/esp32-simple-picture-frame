/*
 * epd.h — 7.3" Waveshare Spectra 6 e-paper display driver
 *
 * Drives the 800×480 six-colour E Ink panel over SPI.  The panel uses a
 * Waveshare proprietary controller with the init sequence and register
 * layout documented in PROGRESS.md (sourced from Waveshare Jan 2026 and
 * aitjcize reference firmware).
 *
 * Typical call sequence
 * ---------------------
 *   pmic_epd_power(pmic, true);      // enable ALDO3 (EPD_VCC)
 *   vTaskDelay(pdMS_TO_TICKS(2));    // allow rail to settle
 *
 *   epd_handle_t epd;
 *   epd_init(&epd);                  // SPI init + hardware reset + panel init
 *
 *   uint8_t *fb = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM);
 *   // ... fill fb with dithered pixel data ...
 *   epd_display(epd, fb, EPD_FB_SIZE); // write frame + trigger ~30 s refresh
 *   heap_caps_free(fb);
 *
 *   epd_sleep(epd);                  // POWER_OFF + DEEP_SLEEP
 *   epd_deinit(epd);                 // release SPI handles
 *   pmic_epd_power(pmic, false);     // disable ALDO3
 *
 * Pixel format
 * ------------
 * 4bpp packed: 2 pixels per byte.  High nibble = x-even pixel,
 * low nibble = x-odd pixel.  Colour indices:
 *
 *   EPD_COLOR_BLACK  = 0
 *   EPD_COLOR_WHITE  = 1
 *   EPD_COLOR_RED    = 2
 *   EPD_COLOR_GREEN  = 3
 *   EPD_COLOR_BLUE   = 4
 *   EPD_COLOR_YELLOW = 5
 *
 * So a solid-white framebuffer is memset(fb, 0x11, EPD_FB_SIZE).
 *
 * Boot-cycle model
 * ----------------
 * epd_init() must be called on every boot — the panel controller loses state
 * across power cycles (including deep sleep).  epd_deinit() releases the SPI
 * bus handles so they can be re-acquired cleanly on the next boot.
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque driver handle.  Allocated by epd_init(), freed by epd_deinit(). */
typedef struct epd_dev_t *epd_handle_t;

/* ── Display geometry ────────────────────────────────────────────────────── */

#define EPD_WIDTH    800u
#define EPD_HEIGHT   480u

/*
 * EPD_FB_SIZE — framebuffer size in bytes.
 *
 * 800 × 480 pixels × 4 bits/pixel = 1,536,000 bits = 192,000 bytes.
 * Allocate with heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM).
 */
#define EPD_FB_SIZE  (EPD_WIDTH * EPD_HEIGHT / 2u)   /* = 192,000 bytes */

/* ── Colour palette indices (Spectra 6) ──────────────────────────────────── */

#define EPD_COLOR_BLACK   0u
#define EPD_COLOR_WHITE   1u
#define EPD_COLOR_RED     2u
#define EPD_COLOR_GREEN   3u
#define EPD_COLOR_BLUE    4u
#define EPD_COLOR_YELLOW  5u

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * epd_init — initialise SPI bus + device, reset panel, send init sequence.
 *
 * Configures GPIO (DC, RST, BUSY), initialises SPI2 at 40 MHz half-duplex,
 * performs a hardware reset, runs the full panel init sequence, and waits
 * for POWER_ON to complete.
 *
 * EPD_VCC (ALDO3) must be enabled by the caller before this function returns.
 * Allow at least 2 ms after enabling ALDO3 before calling epd_init().
 *
 * @param out  Receives the handle on success; untouched on error.
 * @return     ESP_OK, or an error code.
 */
esp_err_t epd_init(epd_handle_t *out);

/*
 * epd_display — write a full frame and trigger a display refresh.
 *
 * Sends 'len' bytes of 4bpp pixel data to the panel SRAM, then runs the
 * refresh sequence (POWER_ON → DISPLAY_REFRESH → POWER_OFF).  The refresh
 * takes approximately 30 seconds; this function blocks until complete.
 *
 * The framebuffer must be exactly EPD_FB_SIZE bytes (192,000).  Allocate
 * in PSRAM with heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM).
 *
 * @param h       Handle from epd_init().
 * @param framebuf  4bpp packed pixel data, EPD_FB_SIZE bytes.
 * @param len     Must equal EPD_FB_SIZE.
 * @return        ESP_OK, or ESP_ERR_INVALID_ARG if len != EPD_FB_SIZE.
 */
esp_err_t epd_display(epd_handle_t h, const uint8_t *framebuf, size_t len);

/*
 * epd_sleep — put the panel controller into deep sleep.
 *
 * Sends POWER_OFF (0x02) → waits BUSY → sends DEEP_SLEEP (0x07, check code
 * 0xA5).  Safe to call whether or not epd_display() was called first.
 *
 * After this call, the panel draws minimal current.  The caller should then
 * call epd_deinit() and disable ALDO3 via pmic_epd_power(pmic, false).
 *
 * @param h  Handle from epd_init().
 * @return   ESP_OK, or ESP_ERR_INVALID_ARG if h is NULL.
 */
esp_err_t epd_sleep(epd_handle_t h);

/*
 * epd_deinit — release SPI device and bus handles.
 *
 * Does not modify panel state.  Call after epd_sleep() and before
 * disabling ALDO3, so the SPI bus is released cleanly before power-down.
 *
 * @param h  Handle from epd_init().  Becomes invalid after this call.
 */
void epd_deinit(epd_handle_t h);

/*
 * epd_run_tests — display solid fills for each of the 6 palette colours.
 *
 * Allocates a 192 KB framebuffer in PSRAM, fills it with each colour in
 * sequence, and calls epd_display() for each.  Each refresh takes ~30 s —
 * allow ~3 minutes for the full suite.  Results require visual verification.
 *
 * @param h  Handle from epd_init().
 * @return   ESP_OK if all displays succeeded, ESP_FAIL otherwise.
 */
esp_err_t epd_run_tests(epd_handle_t h);

#ifdef __cplusplus
}
#endif

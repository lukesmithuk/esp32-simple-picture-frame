/*
 * epd_tests.c — EPD display diagnostics
 *
 * Implements epd_run_tests(), declared in epd.h.
 *
 * Displays a solid fill of each Spectra 6 colour in sequence.  Each fill
 * triggers a full panel refresh (~30 s), so the full suite takes ~3 minutes.
 * Results require visual verification — check that the panel renders each
 * colour cleanly with no artefacts or bleed from adjacent colours.
 *
 * Test lifecycle
 * --------------
 * Each colour fill is run via run_test(), which wraps it with:
 *
 *   setup()    — log test start; the EPD must be idle (BUSY HIGH) entering here,
 *                which is guaranteed because epd_init() and every epd_display()
 *                call both wait for BUSY before returning
 *   test fn()  — fill framebuffer and call epd_display()
 *   teardown() — log result and pause for visual inspection
 *
 * If a test fails (epd_display returns error), the teardown logs the failure.
 * The suite aborts on first failure to avoid pushing frames to a stuck panel.
 */

#include "epd.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* ── Setup / teardown ────────────────────────────────────────────────────── */

static void setup(int n, int total, const char *colour)
{
    /*
     * The EPD must be idle (BUSY HIGH) before each test.  This is guaranteed
     * by the contract of epd_init() and epd_display(): both wait for BUSY
     * before returning.  Log the upcoming test so the tester knows what to
     * expect on the panel.
     */
    ESP_LOGI(TAG, "--- EPD test [%d/%d]: %s ---", n, total, colour);
}

static void teardown(int n, int total, const char *colour, bool passed)
{
    if (passed) {
        ESP_LOGI(TAG, "  [%d/%d] %s — verify visually; continuing in 2 s",
                 n, total, colour);
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        ESP_LOGE(TAG, "  [%d/%d] %s — FAILED; aborting suite", n, total, colour);
    }
}

/* Context passed into each test function */
struct epd_test_ctx {
    epd_handle_t h;
    uint8_t     *fb;
    uint8_t      colour_index;
};

static bool run_test(int n, int total, const char *colour,
                     bool (*fn)(const struct epd_test_ctx *),
                     const struct epd_test_ctx *ctx)
{
    setup(n, total, colour);
    bool result = fn(ctx);
    teardown(n, total, colour, result);
    return result;
}

/* ── Individual test function ────────────────────────────────────────────── */

static bool test_solid_fill(const struct epd_test_ctx *ctx)
{
    /*
     * Pixel format: 4bpp packed, two pixels per byte, high nibble first.
     * For a solid fill, both nibbles hold the same colour index.
     */
    uint8_t packed = (uint8_t)((ctx->colour_index << 4) | ctx->colour_index);
    memset(ctx->fb, packed, EPD_FB_SIZE);

    ESP_LOGI(TAG, "  index %u, byte 0x%02X — refreshing...", ctx->colour_index, packed);

    esp_err_t err = epd_display(ctx->h, ctx->fb, EPD_FB_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  epd_display failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

esp_err_t epd_run_tests(epd_handle_t h)
{
    ESP_LOGI(TAG, "--- EPD tests: solid colour fills (6 colours × ~30 s each) ---");

    /*
     * The framebuffer is 192 KB — too large for DRAM.  MALLOC_CAP_SPIRAM
     * places it in the 8 MB PSRAM.  On ESP32-S3, PSRAM is DMA-accessible
     * via EDMA, so this buffer can be passed directly to the SPI driver.
     */
    uint8_t *fb = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!fb) {
        ESP_LOGE(TAG, "  [FAIL] could not allocate %u-byte framebuffer in PSRAM",
                 EPD_FB_SIZE);
        return ESP_ERR_NO_MEM;
    }

    static const struct { uint8_t index; const char *name; } colours[] = {
        { EPD_COLOR_BLACK,  "black"  },
        { EPD_COLOR_WHITE,  "white"  },
        { EPD_COLOR_RED,    "red"    },
        { EPD_COLOR_GREEN,  "green"  },
        { EPD_COLOR_BLUE,   "blue"   },
        { EPD_COLOR_YELLOW, "yellow" },
    };
    const int total = (int)(sizeof(colours) / sizeof(colours[0]));

    bool pass = true;
    for (int i = 0; i < total; i++) {
        struct epd_test_ctx ctx = {
            .h            = h,
            .fb           = fb,
            .colour_index = colours[i].index,
        };
        if (!run_test(i + 1, total, colours[i].name, test_solid_fill, &ctx)) {
            pass = false;
            break;
        }
    }

    heap_caps_free(fb);

    if (pass)
        ESP_LOGI(TAG, "=== EPD tests: COMPLETE (visual verification required) ===");
    else
        ESP_LOGE(TAG, "=== EPD tests: FAILED ===");

    return pass ? ESP_OK : ESP_FAIL;
}

/*
 * epd_tests.c — EPD display diagnostics
 *
 * Implements epd_run_tests(), declared in epd.h.
 *
 * Displays a solid fill of each Spectra 6 colour in sequence.  Each fill
 * triggers a full panel refresh (~30 s), so the full suite takes ~3 minutes.
 * Results require visual verification — check that the panel renders each
 * colour cleanly with no artefacts or bleed from adjacent colours.
 */

#include "epd.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

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

    /*
     * All six Spectra 6 palette colours, in display order.  Each entry maps
     * a colour index constant (defined in epd.h) to a human-readable name
     * for the log output.
     */
    static const struct { uint8_t index; const char *name; } colours[] = {
        { EPD_COLOR_BLACK,  "black"  },
        { EPD_COLOR_WHITE,  "white"  },
        { EPD_COLOR_RED,    "red"    },
        { EPD_COLOR_GREEN,  "green"  },
        { EPD_COLOR_BLUE,   "blue"   },
        { EPD_COLOR_YELLOW, "yellow" },
    };

    bool pass = true;
    for (int i = 0; i < (int)(sizeof(colours) / sizeof(colours[0])); i++) {
        uint8_t idx    = colours[i].index;
        /*
         * Pixel format is 4bpp packed: two pixels per byte, high nibble
         * first.  For a solid fill, both nibbles hold the same index,
         * so the packed byte is (idx << 4) | idx.
         */
        uint8_t packed = (uint8_t)((idx << 4) | idx);
        memset(fb, packed, EPD_FB_SIZE);

        ESP_LOGI(TAG, "  [%d/6] %s (index %u, byte 0x%02X) — refreshing...",
                 i + 1, colours[i].name, idx, packed);

        esp_err_t err = epd_display(h, fb, EPD_FB_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  [FAIL] epd_display: %s", esp_err_to_name(err));
            pass = false;
            break;
        }

        /*
         * Brief pause after each refresh so the tester can inspect the
         * panel before the next colour overwrites it.  epd_display() has
         * already waited for the BUSY line, so the panel is idle here.
         */
        ESP_LOGI(TAG, "  [%d/6] %s — verify visually, continuing in 2 s",
                 i + 1, colours[i].name);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    heap_caps_free(fb);

    if (pass)
        ESP_LOGI(TAG, "=== EPD tests: COMPLETE (visual verification required) ===");
    else
        ESP_LOGE(TAG, "=== EPD tests: FAILED ===");

    return pass ? ESP_OK : ESP_FAIL;
}

#include "test_main.h"

#include <string.h>

#include "board.h"
#include "epd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "test";

#define PASS(name) ESP_LOGI(TAG, "PASS: %s", name)
#define FAIL(name, reason) ESP_LOGE(TAG, "FAIL: %s — %s", name, reason)
#define CHECK(name, expr) \
    do { \
        if ((expr) == ESP_OK) { PASS(name); } \
        else { FAIL(name, esp_err_to_name(expr)); } \
    } while (0)

/* ── Test cases ─────────────────────────────────────────────────────────── */

static void test_board_pmic(void)
{
    /* board_init() already called — PMIC was initialised as part of that.
     * Just verify EPD power toggle works. */
    esp_err_t ret;

    ret = board_epd_power(true);
    CHECK("board_epd_power ON", ret);
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = board_epd_power(false);
    CHECK("board_epd_power OFF", ret);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void test_board_rtc(void)
{
    if (!board_rtc_is_available()) {
        FAIL("board_rtc_available", "PCF85063 not detected");
        return;
    }
    PASS("board_rtc_available");

    time_t t;
    esp_err_t ret = board_rtc_get_time(&t);
    if (ret == ESP_OK) {
        PASS("board_rtc_get_time");
        ESP_LOGI(TAG, "  RTC epoch: %lld", (long long)t);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        /* Oscillator-stop flag set — clock was never set, not a driver error */
        ESP_LOGW(TAG, "WARN: board_rtc_get_time — OSF set (clock never set)");
    } else {
        FAIL("board_rtc_get_time", esp_err_to_name(ret));
    }
}

static void test_epd_color_bars(void)
{
    static const epd_color_t colors[] = {
        EPD_COLOR_BLACK, EPD_COLOR_WHITE, EPD_COLOR_GREEN,
        EPD_COLOR_BLUE,  EPD_COLOR_RED,   EPD_COLOR_YELLOW,
        EPD_COLOR_ORANGE,
    };
    static const char *names[] = {
        "black", "white", "green", "blue", "red", "yellow", "orange",
    };
    const int num_colors = sizeof(colors) / sizeof(colors[0]);

    CHECK("board_epd_power ON", board_epd_power(true));
    CHECK("epd_init", epd_init());

    uint8_t *fb = epd_alloc_frame_buf();
    if (!fb) {
        FAIL("epd_alloc_frame_buf", "returned NULL");
        epd_deinit();
        board_epd_power(false);
        return;
    }
    PASS("epd_alloc_frame_buf");

    /* Fill horizontal stripes, one per palette color */
    const int stripe_h = EPD_HEIGHT / num_colors;
    for (int c = 0; c < num_colors; c++) {
        uint8_t byte = (colors[c] << 4) | colors[c];
        int y_start = c * stripe_h;
        int y_end = (c == num_colors - 1) ? EPD_HEIGHT : y_start + stripe_h;
        memset(&fb[y_start * (EPD_WIDTH / 2)], byte,
               (y_end - y_start) * (EPD_WIDTH / 2));
        ESP_LOGI(TAG, "  Stripe %d: rows %d–%d = %s", c, y_start, y_end - 1, names[c]);
    }

    ESP_LOGI(TAG, "Displaying color bars ...");
    CHECK("epd_display_color_bars", epd_display(fb));

    free(fb);
    epd_deinit();
    board_epd_power(false);
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void tests_run(void)
{
    ESP_LOGI(TAG, "=== Hardware Integration Tests ===");

    test_board_pmic();
    test_board_rtc();
    test_epd_color_bars();

    ESP_LOGI(TAG, "=== Tests complete ===");
}

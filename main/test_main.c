#include "test_main.h"

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

static void test_epd_solid_colors(void)
{
    static const struct { epd_color_t color; const char *name; } colors[] = {
        { EPD_COLOR_WHITE,  "white"  },
        { EPD_COLOR_BLACK,  "black"  },
        { EPD_COLOR_RED,    "red"    },
        { EPD_COLOR_GREEN,  "green"  },
        { EPD_COLOR_BLUE,   "blue"   },
        { EPD_COLOR_YELLOW, "yellow" },
    };

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

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        ESP_LOGI(TAG, "Displaying solid %s ...", colors[i].name);
        epd_fill_color(fb, colors[i].color);
        esp_err_t ret = epd_display(fb);
        char test_name[32];
        snprintf(test_name, sizeof(test_name), "epd_display_%s", colors[i].name);
        CHECK(test_name, ret);
        /* Pause between colours so display can be observed */
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

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
    test_epd_solid_colors();

    ESP_LOGI(TAG, "=== Tests complete ===");
}

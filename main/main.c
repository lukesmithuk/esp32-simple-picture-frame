#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "epd.h"

#ifdef CONFIG_TEST_MODE
#include "test_main.h"
#endif

static const char *TAG = "main";

/* Minutes between wakeups. TODO: make configurable / load from SD. */
#define WAKE_INTERVAL_MINUTES 1

static void set_next_alarm(void)
{
    if (!board_rtc_is_available())
        return;

    time_t now;
    if (board_rtc_get_time(&now) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read RTC — skipping alarm set");
        return;
    }

    time_t next = now + (WAKE_INTERVAL_MINUTES * 60);
    struct tm t;
    localtime_r(&next, &t);

    esp_err_t ret = board_rtc_set_alarm(t.tm_hour, t.tm_min);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set alarm: %s", esp_err_to_name(ret));
    }
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Woke from deep sleep (RTC alarm)");
    } else {
        ESP_LOGI(TAG, "esp32-picture-frame starting (cold boot)");
    }

    ESP_ERROR_CHECK(board_init());

    /* Clear alarm flag from previous wake (or stale flag from cold boot) */
    board_rtc_clear_alarm_flag();

#ifdef CONFIG_TEST_MODE
    tests_run();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    /* Production path -------------------------------------------------- */
    ESP_ERROR_CHECK(board_epd_power(true));
    ESP_ERROR_CHECK(epd_init());

    uint8_t *frame_buf = epd_alloc_frame_buf();
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto sleep;
    }

    /* TODO: load image from SD card and dither into frame_buf.
     * For now display a solid white test pattern. */
    epd_fill_color(frame_buf, EPD_COLOR_WHITE);

    ESP_LOGI(TAG, "Displaying frame");
    esp_err_t ret = epd_display(frame_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "epd_display failed: %s", esp_err_to_name(ret));
    }

sleep:
    free(frame_buf);    /* free(NULL) is a no-op per C99 */
    epd_deinit();
    board_epd_power(false);

#ifdef CONFIG_DISABLE_DEEP_SLEEP
    ESP_LOGI(TAG, "Deep sleep disabled — halting");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#else
    set_next_alarm();
    board_enter_deep_sleep();
    /* Does not return */
#endif
#endif
}

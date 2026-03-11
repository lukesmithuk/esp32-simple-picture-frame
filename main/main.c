#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "epd.h"

#ifdef CONFIG_TEST_MODE
#include "test_main.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "esp32-picture-frame starting");

    ESP_ERROR_CHECK(board_init());

#ifdef CONFIG_TEST_MODE
    tests_run();
    /* Test mode: loop forever so output can be read. */
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

    /* TODO Phase N: configure RTC alarm, then board_sleep() + esp_deep_sleep_start() */
    ESP_LOGI(TAG, "Done — halting (deep sleep not yet wired)");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#endif
}

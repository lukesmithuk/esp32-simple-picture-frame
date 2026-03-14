#include <time.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "epd.h"
#include "sdcard.h"
#include "image_picker.h"
#include "image_loader.h"
#include "epd_text.h"
#include "errlog.h"

#ifdef CONFIG_TEST_MODE
#include "test_main.h"
#endif

static const char *TAG = "main";
static const char *image_exts[] = {"jpg", "jpeg", NULL};

#define IMAGE_DIR  SDCARD_MOUNT_POINT "/images"
#define ERROR_LOG  SDCARD_MOUNT_POINT "/error.log"

#ifndef CONFIG_DISABLE_DEEP_SLEEP
/* Wake interval. TODO: make configurable / load from SD. */
#define WAKE_INTERVAL_HOURS   1
#define WAKE_INTERVAL_MINUTES 0
#define WAKE_INTERVAL_SECONDS 0

static void set_next_alarm(void)
{
    if (!board_rtc_is_available())
        return;

    time_t now;
    if (board_rtc_get_time(&now) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read RTC — skipping alarm set");
        return;
    }

    time_t next = now + WAKE_INTERVAL_HOURS * 3600
                      + WAKE_INTERVAL_MINUTES * 60
                      + WAKE_INTERVAL_SECONDS;
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGD(TAG, "NOW : %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    localtime_r(&next, &t);
    ESP_LOGD(TAG, "NEXT: %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);

    esp_err_t ret = board_rtc_set_alarm(t.tm_hour, t.tm_min, t.tm_sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set alarm: %s", esp_err_to_name(ret));
    }
}
#endif

static void show_error(uint8_t *frame_buf, const char *message)
{
    epd_fill_color(frame_buf, EPD_COLOR_WHITE);
    epd_text_draw_centred(frame_buf, EPD_HEIGHT / 2 - 16,
                          message, EPD_COLOR_RED, EPD_COLOR_WHITE, 3);
    epd_display(frame_buf);
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

    /* Log current RTC time */
    if (board_rtc_is_available()) {
        time_t now;
        if (board_rtc_get_time(&now) == ESP_OK) {
            struct tm t;
            localtime_r(&now, &t);
            ESP_LOGI(TAG, "RTC time: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        }
    }

    /* Clear alarm flag from previous wake (or stale flag from cold boot) */
    board_rtc_clear_alarm_flag();

#ifdef CONFIG_TEST_MODE
    tests_run();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    /* Production path -------------------------------------------------- */
    uint8_t *frame_buf = NULL;
    uint8_t *img_buf = NULL;
    bool sd_mounted = false;
    esp_err_t ret;

    ESP_ERROR_CHECK(board_epd_power(true));
    ESP_ERROR_CHECK(epd_init());

    frame_buf = epd_alloc_frame_buf();
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto sleep;
    }

    /* Mount SD card */
    ret = sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed");
        show_error(frame_buf, "No SD card");
        goto sleep;
    }
    sd_mounted = true;

    /* Pick a random image */
    char img_path[IMAGE_PICKER_PATH_MAX];
    ret = image_picker_pick(IMAGE_DIR, image_exts, img_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No images found in " IMAGE_DIR);
        errlog_write(ERROR_LOG, "No images found in " IMAGE_DIR);
        show_error(frame_buf, "No images found");
        goto unmount;
    }

    /* Load image file into PSRAM */
    size_t img_size = 0;
    ret = image_loader_load(img_path, &img_buf, &img_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load: %s", img_path);
        errlog_write(ERROR_LOG, "Failed to load image");
        show_error(frame_buf, "Image load error");
        goto unmount;
    }

    /* TODO Phase 7: JPEG decode + scale + dither img_buf → frame_buf.
     * For now display a white placeholder to prove the pipeline works. */
    ESP_LOGI(TAG, "Loaded %u bytes from %s (decode not yet implemented)",
             img_size, img_path);
    epd_fill_color(frame_buf, EPD_COLOR_WHITE);

    ESP_LOGI(TAG, "Displaying frame");
    ret = epd_display(frame_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "epd_display failed: %s", esp_err_to_name(ret));
    }

unmount:
    free(img_buf);      /* free(NULL) is a no-op per C99 */
    if (sd_mounted) {
        sdcard_unmount();
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

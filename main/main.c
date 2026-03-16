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
#include "applog.h"
#include "config.h"
#include "image_decode.h"

#ifdef CONFIG_TEST_MODE
#include "test_main.h"
#endif

static const char *TAG = "main";
static const char *image_exts[] = {"jpg", "jpeg", NULL};

#define IMAGE_DIR    SDCARD_MOUNT_POINT "/images"
#define SYSTEM_LOG   SDCARD_MOUNT_POINT "/system.log"
#define CONFIG_PATH  SDCARD_MOUNT_POINT "/config.txt"

#ifndef CONFIG_DISABLE_DEEP_SLEEP
static void set_next_alarm(void)
{
    if (!board_rtc_is_available())
        return;

    time_t now;
    if (board_rtc_get_time(&now) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read RTC — skipping alarm set");
        return;
    }

    int hours   = config_get_int("wake_interval_hours", 1);
    int minutes = config_get_int("wake_interval_minutes", 0);
    int seconds = config_get_int("wake_interval_seconds", 0);
    ESP_LOGI(TAG, "Wake interval: %dh %dm %ds", hours, minutes, seconds);

    time_t next = now + hours * 3600 + minutes * 60 + seconds;
    struct tm t;
    localtime_r(&next, &t);
    ESP_LOGI(TAG, "Next alarm: %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);

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
                          message, EPD_COLOR_BLACK, EPD_COLOR_WHITE, 4);
    epd_display(frame_buf);
}

void app_main(void)
{
    applog_init();

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

    /* Log battery / power status */
    int batt_mv = board_battery_voltage_mv();
    if (board_battery_is_connected() && batt_mv > 1000) {
        ESP_LOGI(TAG, "Battery: %d%% (%d mV)%s",
                 board_battery_percent(), batt_mv,
                 board_battery_is_charging() ? " [charging]" : "");
    } else {
        ESP_LOGI(TAG, "No battery (USB: %s)",
                 board_usb_is_connected() ? "yes" : "no");
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

    /* Load config first — needed for log rolling threshold and WiFi settings. */
    config_load(CONFIG_PATH);

    /* Start logging all ESP_LOG output to SD card (with rolling). */
    int log_max_kb = config_get_int("log_max_size_kb", 256);
    applog_start(SYSTEM_LOG, log_max_kb);

    /* Pick a random image */
    char img_path[IMAGE_PICKER_PATH_MAX];
    ret = image_picker_pick(IMAGE_DIR, image_exts, img_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No images found in " IMAGE_DIR);
        show_error(frame_buf, "No images found");
        goto unmount;
    }

    /* Load image file into PSRAM */
    size_t img_size = 0;
    ret = image_loader_load(img_path, &img_buf, &img_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load: %s", img_path);
        show_error(frame_buf, "Image load error");
        goto unmount;
    }

    /* JPEG decode → scale → dither into frame buffer */
    ESP_LOGI(TAG, "Decoding %zu bytes from %s", img_size, img_path);
    ret = image_decode_jpeg(img_buf, img_size, frame_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Decode failed: %s", esp_err_to_name(ret));
        show_error(frame_buf, "Decode error");
        goto unmount;
    }

    ESP_LOGI(TAG, "Displaying frame");
    ret = epd_display(frame_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "epd_display failed: %s", esp_err_to_name(ret));
    }

unmount:
    free(img_buf);      /* free(NULL) is a no-op per C99 */
    if (sd_mounted) {
        applog_stop();
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
    set_next_alarm();   /* reads config values from RAM — safe after SD unmount */
    board_enter_deep_sleep();
    /* Does not return */
#endif
#endif
}

#include <time.h>

#include "esp_app_desc.h"
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
#include "wifi_fetch.h"

#ifdef CONFIG_TEST_MODE
#include "test_main.h"
#endif

static const char *TAG = "main";
static const char *image_exts[] = {"jpg", "jpeg", NULL};

#define IMAGE_DIR       SDCARD_MOUNT_POINT "/images"
#define SYSTEM_LOG      SDCARD_MOUNT_POINT "/system.log"
#define CONFIG_PATH     SDCARD_MOUNT_POINT "/config.txt"
#define LOG_OFFSET_PATH SDCARD_MOUNT_POINT "/.log_offset"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

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

    /* Use server-provided wake interval if available, else config.txt. */
    int hours, minutes, seconds;
    if (!wifi_fetch_get_wake_interval(&hours, &minutes, &seconds)) {
        hours   = config_get_int("wake_interval_hours", 1);
        minutes = config_get_int("wake_interval_minutes", 0);
        seconds = config_get_int("wake_interval_seconds", 0);
        ESP_LOGI(TAG, "Wake interval (config): %dh %dm %ds", hours, minutes, seconds);
    } else {
        ESP_LOGI(TAG, "Wake interval (server): %dh %dm %ds", hours, minutes, seconds);
    }

    int total_seconds = hours * 3600 + minutes * 60 + seconds;
    if (total_seconds <= 0) {
        ESP_LOGW(TAG, "Wake interval %ds invalid, using default 1h", total_seconds);
        hours = 1;
        minutes = 0;
        seconds = 0;
        total_seconds = 3600;
    }

    time_t next = now + total_seconds;
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

static void suppress_noisy_logs(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
}

static void log_boot_info(void)
{
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

    int batt_mv = board_battery_voltage_mv();
    if (board_battery_is_connected() && batt_mv > 1000) {
        ESP_LOGI(TAG, "Battery: %d%% (%d mV)%s",
                 board_battery_percent(), batt_mv,
                 board_battery_is_charging() ? " [charging]" : "");
    } else {
        ESP_LOGI(TAG, "No battery (USB: %s)",
                 board_usb_is_connected() ? "yes" : "no");
    }
}

/* ── WiFi image fetch ────────────────────────────────────────────────────── */

/**
 * Try to fetch an image from the server over WiFi.
 * Also uploads logs and pushes status.
 * Returns ESP_OK with img_buf/img_size set on success.
 */
static esp_err_t try_wifi_fetch(uint8_t **img_buf, size_t *img_size)
{
    const char *wifi_ssid = config_get_str("wifi_ssid", NULL);
    if (!wifi_ssid)
        return ESP_ERR_NOT_SUPPORTED;

    const char *wifi_pass  = config_get_str("wifi_password", "");
    const char *server_url = config_get_str("server_url", "");
    const char *api_key    = config_get_str("server_api_key", "");

    esp_err_t ret = wifi_fetch_init(wifi_ssid, wifi_pass);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed");
        return ret;
    }

    /* Upload logs BEFORE applog_start (which does rolling). */
    wifi_fetch_post_logs(server_url, api_key, SYSTEM_LOG, LOG_OFFSET_PATH);

    /* Start log capture (with rolling) now that logs are uploaded. */
    int log_max_kb = config_get_int("log_max_size_kb", 256);
    applog_start(SYSTEM_LOG, log_max_kb);

    /* Push frame status. */
    int batt_mv = board_battery_voltage_mv();
    wifi_fetch_status_t status = {
        .battery_connected = board_battery_is_connected() && batt_mv > 1000,
        .battery_percent   = board_battery_percent(),
        .battery_mv        = batt_mv,
        .charging          = board_battery_is_charging(),
        .usb_connected     = board_usb_is_connected(),
        .sd_free_kb        = 0,  /* TODO: implement sd_free_kb */
        .firmware_version  = esp_app_get_description()->version,
    };
    wifi_fetch_post_status(server_url, api_key, &status);

    /* Fetch next image. */
    ret = wifi_fetch_image(server_url, api_key, img_buf, img_size);
    wifi_fetch_deinit();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Got %zu bytes from server", *img_size);
    }
    return ret;
}

/* ── SD card image fetch ─────────────────────────────────────────────────── */

static esp_err_t try_sd_fetch(uint8_t **img_buf, size_t *img_size)
{
    char img_path[IMAGE_PICKER_PATH_MAX];
    esp_err_t ret = image_picker_pick(IMAGE_DIR, image_exts, img_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No images found in " IMAGE_DIR);
        return ret;
    }

    ret = image_loader_load(img_path, img_buf, img_size);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Loaded %zu bytes from SD: %s", *img_size, img_path);
    return ret;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    applog_init();
    suppress_noisy_logs();

    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Woke from deep sleep (RTC alarm)");
    } else {
        ESP_LOGI(TAG, "esp32-picture-frame starting (cold boot)");
    }

    ESP_ERROR_CHECK(board_init());
    log_boot_info();
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
    size_t img_size = 0;
    bool sd_mounted = false;
    esp_err_t ret;

    ESP_ERROR_CHECK(board_epd_power(true));
    ESP_ERROR_CHECK(epd_init());

    frame_buf = epd_alloc_frame_buf();
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto sleep;
    }

    /* Mount SD card (required for config, logs, and fallback images). */
    ret = sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed");
        show_error(frame_buf, "No SD card");
        goto sleep;
    }
    sd_mounted = true;

    /* Load config — needed for WiFi, log rolling, wake interval. */
    config_load(CONFIG_PATH);

    /* Try WiFi first, fall back to SD card. */
    ret = try_wifi_fetch(&img_buf, &img_size);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NOT_SUPPORTED)
            ESP_LOGW(TAG, "WiFi fetch failed, trying SD card");

        /* Start log capture if WiFi path didn't (no-WiFi or connect failed). */
        int log_max_kb = config_get_int("log_max_size_kb", 256);
        applog_start(SYSTEM_LOG, log_max_kb);

        ret = try_sd_fetch(&img_buf, &img_size);
        if (ret != ESP_OK) {
            show_error(frame_buf, "No images found");
            goto unmount;
        }
    }

    /* Decode and display. */
    ESP_LOGI(TAG, "Decoding %zu bytes", img_size);
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
    free(img_buf);
    if (sd_mounted) {
        applog_stop();
        sdcard_unmount();
    }

sleep:
    free(frame_buf);
    epd_deinit();
    board_epd_power(false);

#ifdef CONFIG_DISABLE_DEEP_SLEEP
    ESP_LOGI(TAG, "Deep sleep disabled — halting");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#else
    set_next_alarm();   /* reads config values from RAM — safe after SD unmount */
    board_sleep();      /* PMIC low-power mode — cuts all rails except DCDC1 */
    board_enter_deep_sleep();
    /* Does not return */
#endif
#endif
}

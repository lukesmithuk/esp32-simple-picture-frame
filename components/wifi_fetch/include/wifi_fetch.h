#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  battery_connected;
    int   battery_percent;
    int   battery_mv;
    bool  charging;
    bool  usb_connected;
    int   sd_free_kb;
    const char *firmware_version;
} wifi_fetch_status_t;

/**
 * @brief Connect to WiFi and sync time via NTP → RTC.
 *
 * Blocks up to 10 seconds for WiFi association, then up to 5 seconds
 * for NTP sync. RTC is updated on successful NTP sync.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if WiFi connect fails.
 */
esp_err_t wifi_fetch_init(const char *ssid, const char *password);

/**
 * @brief Fetch the next image from the server.
 *
 * GET /api/next with API key and frame MAC in headers.
 * Pre-allocates PSRAM buffer based on Content-Length (max 4 MB).
 * Caller must free *out_buf.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if gallery empty (204),
 *         ESP_FAIL on error.
 */
esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size);

/**
 * @brief Push frame status to server as JSON.
 *
 * POST /api/status. Caller builds the status struct from board APIs.
 */
esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key,
                                 const wifi_fetch_status_t *status);

/**
 * @brief Push new log lines since last upload.
 *
 * Reads from log_path starting at the byte offset stored in offset_path.
 * Updates offset_path on success. Caps upload at 16 KB per call.
 *
 * @return ESP_OK on success, ESP_FAIL on error (non-fatal, retry next cycle).
 */
esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path);

/**
 * @brief Get the wake interval from the last /api/next response.
 *
 * Returns true if the server provided wake interval headers.
 * Values are only valid after a successful wifi_fetch_image() call.
 */
bool wifi_fetch_get_wake_interval(int *hours, int *minutes, int *seconds);

/**
 * @brief Disconnect WiFi and release resources.
 */
void wifi_fetch_deinit(void);

#ifdef __cplusplus
}
#endif

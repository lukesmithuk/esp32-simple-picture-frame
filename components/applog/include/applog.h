#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Begin buffering ESP_LOG output to RAM.
 *
 * Call early in app_main(), before the SD card is mounted.  All ESP_LOG
 * messages are buffered (up to 4 KB) and still printed to serial.
 * The buffer is flushed to file when applog_start() is called.
 */
void applog_init(void);

/**
 * @brief Start capturing all ESP_LOG output to a file.
 *
 * Opens the file in append mode, flushes any buffered early-boot
 * messages, and switches to writing all ESP_LOG* output to both
 * serial and the file.  Call applog_stop() before unmounting the
 * SD card.
 *
 * @param log_path  Absolute path, e.g. "/sdcard/system.log".
 * @return ESP_OK on success, ESP_FAIL if the file cannot be opened.
 */
esp_err_t applog_start(const char *log_path);

/**
 * @brief Stop capturing ESP_LOG output to file.
 *
 * Restores the original vprintf handler and closes the log file.
 * Safe to call even if applog_start() was not called or failed.
 */
void applog_stop(void);

/**
 * @brief Append a timestamped line to a log file.
 *
 * Opens the file in append mode, writes one line, and closes it.
 * Creates the file if it does not exist.
 *
 * Format: "[YYYY-MM-DD HH:MM:SS] message\n"
 * If the system clock is not set (year < 2020), uses "[NO-RTC]" instead.
 *
 * @param log_path  Absolute path, e.g. "/sdcard/error.log".
 * @param message   Message string (no trailing newline needed).
 *
 * @return ESP_OK on success, ESP_FAIL if the file cannot be opened.
 */
esp_err_t applog_write(const char *log_path, const char *message);

#ifdef __cplusplus
}
#endif

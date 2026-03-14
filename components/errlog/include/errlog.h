#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
esp_err_t errlog_write(const char *log_path, const char *message);

#ifdef __cplusplus
}
#endif

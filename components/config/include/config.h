#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load a key=value config file from the given path.
 *
 * Parses lines in the format "key=value". Leading/trailing whitespace
 * on keys and values is trimmed. Lines starting with '#' are comments.
 * Blank lines are ignored. Values are stored internally and accessed
 * via the getter functions below.
 *
 * @param path  Absolute file path, e.g. "/sdcard/config.txt".
 * @return ESP_OK on success (including file-not-found, which uses defaults).
 *         ESP_FAIL on read error.
 */
esp_err_t config_load(const char *path);

/**
 * @brief Get an integer config value, or a default if not set.
 */
int config_get_int(const char *key, int default_value);

/**
 * @brief Get a string config value, or a default if not set.
 *
 * Returns a pointer to internal storage — valid until the next
 * config_load() call.
 */
const char *config_get_str(const char *key, const char *default_value);

#ifdef __cplusplus
}
#endif

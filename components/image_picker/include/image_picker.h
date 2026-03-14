#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_PICKER_PATH_MAX 256

/**
 * @brief Scan a directory for image files and pick one at random.
 *
 * Enumerates files whose extension (case-insensitive) matches any entry
 * in the null-terminated @p exts array. Selects one uniformly at random.
 *
 * @param dir_path  Absolute directory path, e.g. "/sdcard/images".
 * @param exts      Null-terminated array of extensions without dot,
 *                  e.g. {"jpg", "jpeg", NULL}.
 * @param out_path  Buffer of at least IMAGE_PICKER_PATH_MAX bytes;
 *                  filled with the full path of the chosen file.
 *
 * @return ESP_OK            on success.
 *         ESP_ERR_NOT_FOUND if no matching files found.
 *         ESP_FAIL          if the directory cannot be opened.
 */
esp_err_t image_picker_pick(const char *dir_path,
                            const char *const *exts,
                            char *out_path);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reject files larger than this to prevent PSRAM exhaustion. */
#define IMAGE_LOADER_MAX_FILE_BYTES (4 * 1024 * 1024)

/**
 * @brief Load a file into a PSRAM buffer.
 *
 * Reads the entire file at @p path into a newly allocated PSRAM buffer.
 * Caller must free() the returned buffer.
 *
 * @param path      Absolute file path.
 * @param out_buf   Set to the allocated buffer, or NULL on failure.
 * @param out_size  Set to the file size in bytes.
 *
 * @return ESP_OK          on success.
 *         ESP_ERR_NO_MEM  if file too large or PSRAM allocation failed.
 *         ESP_FAIL        on I/O error.
 */
esp_err_t image_loader_load(const char *path,
                            uint8_t **out_buf, size_t *out_size);

#ifdef __cplusplus
}
#endif

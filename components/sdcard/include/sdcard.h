#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_MOUNT_POINT "/sdcard"

/**
 * @brief Mount the SD card via 4-bit SDIO at /sdcard.
 *
 * Idempotent — safe to call if already mounted.
 *
 * @return ESP_OK on success, ESP_FAIL on mount error,
 *         ESP_ERR_NOT_FOUND if no card detected.
 */
esp_err_t sdcard_mount(void);

/**
 * @brief Unmount the SD card.
 *
 * Idempotent — safe to call if not mounted.
 * Returns void because unmount errors are non-fatal (logged but not propagated).
 */
void sdcard_unmount(void);

/** @brief Returns true if the SD card is currently mounted. */
bool sdcard_is_mounted(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode a JPEG and render it into a 4bpp EPD frame buffer.
 *
 * Pipeline: JPEG decode (TJpgDec via ROM) → nearest-neighbor scale to
 * 800×480 (cover mode, centre-crop) → Floyd-Steinberg dither to the
 * 6-colour Spectra 6 palette → 4bpp packed frame buffer.
 *
 * @param jpeg_buf   Raw JPEG bytes (typically PSRAM, from image_loader_load).
 * @param jpeg_size  Size of jpeg_buf in bytes.
 * @param frame_buf  Caller-allocated EPD_BUF_SIZE (192 000 B) buffer.
 *                   Fully overwritten on success; undefined on failure.
 *
 * @return ESP_OK              on success.
 *         ESP_ERR_INVALID_ARG if any pointer is NULL or jpeg_size is 0.
 *         ESP_ERR_NO_MEM      if intermediate buffer allocation fails.
 *         ESP_FAIL            if JPEG decode fails (corrupt/progressive JPEG).
 */
esp_err_t image_decode_jpeg(const uint8_t *jpeg_buf, size_t jpeg_size,
                            uint8_t *frame_buf);

#ifdef __cplusplus
}
#endif

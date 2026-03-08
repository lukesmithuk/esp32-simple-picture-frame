/*
 * epd_internal.h — private implementation details for the epd component.
 *
 * Included only by epd.c.  Do not include from outside the component —
 * use epd.h for the public API.
 */

#pragma once

#include "driver/spi_master.h"

/* ── GPIO pin assignments (confirmed from schematic 2026-02-21) ──────────── */

#define EPD_PIN_MOSI   11
#define EPD_PIN_CLK    10
#define EPD_PIN_CS      9
#define EPD_PIN_DC      8
#define EPD_PIN_RST    12
#define EPD_PIN_BUSY   13   /* active LOW: LOW = busy, HIGH = idle */

/* ── SPI configuration ───────────────────────────────────────────────────── */

#define EPD_SPI_HOST      SPI2_HOST
#define EPD_SPI_FREQ_HZ   (10 * 1000 * 1000)   /* 10 MHz */

/*
 * EPD_CHUNK_SIZE — bytes per SPI sub-transaction when streaming frame data.
 *
 * The IDF SPI driver has a per-transaction hardware limit well below the
 * 192 KB framebuffer, so the frame is sent in chunks.  CS is driven as a
 * plain GPIO (spics_io_num = -1) and held LOW for the entire frame across
 * all chunks, so the panel sees one continuous data stream.
 *
 * 4000 bytes matches aitjcize/esp32-photoframe exactly.  Combined with
 * vTaskDelay(1) after every chunk, this gives the panel ~10 ms to drain its
 * internal SPI→SRAM DMA pipeline between bursts.  Larger chunks (e.g. 5000)
 * with infrequent yields cause silent SPI buffer overflow on the panel side,
 * resulting in a corrupted frame that never refreshes correctly.
 */
#define EPD_CHUNK_SIZE    5000u

/* ── Driver state ────────────────────────────────────────────────────────── */

struct epd_dev_t {
    spi_device_handle_t spi_dev;
};

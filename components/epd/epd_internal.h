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
#define EPD_SPI_FREQ_HZ   (40 * 1000 * 1000)   /* 40 MHz, per Waveshare spec */

/*
 * EPD_CHUNK_SIZE — bytes per SPI transaction when streaming frame data.
 *
 * Both reference implementations (aitjcize and Waveshare Jan 2026) use 5000-
 * byte chunks.  A single large DMA transfer of the full 192 KB frame may be
 * possible on ESP32-S3 with EDMA (PSRAM → SPI2) — see TODO.md backlog.
 */
#define EPD_CHUNK_SIZE    5000u

/* ── Driver state ────────────────────────────────────────────────────────── */

struct epd_dev_t {
    spi_device_handle_t spi_dev;
};

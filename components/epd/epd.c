/*
 * epd.c — 7.3" Waveshare Spectra 6 e-paper display driver
 *
 * See epd.h for the public API and usage notes.
 * See epd_internal.h for pin assignments and SPI config.
 * See epd_tests.c for epd_run_tests().
 *
 * Init sequence and refresh sequence sourced from Waveshare Jan 2026
 * display_bsp.cpp and cross-checked against aitjcize/esp32-photoframe.
 * All bytes confirmed in PROGRESS.md.
 */

#include "epd.h"
#include "epd_internal.h"

#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

/*
 * epd_wait_busy — block until the BUSY pin goes HIGH (panel idle).
 *
 * BUSY is active LOW: the panel holds it LOW while processing commands.
 * Polls every 10 ms with vTaskDelay so other FreeRTOS tasks can run during
 * the ~30 s display refresh.  Logs progress every 5 s at DEBUG level.
 * Gives up after 60 s and logs an error (panel may be absent or stuck).
 */
static void epd_wait_busy(void)
{
    const int POLL_MS    = 10;
    const int LOG_MS     = 5000;
    const int TIMEOUT_MS = 60000;
    int elapsed_ms = 0;

    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        elapsed_ms += POLL_MS;
        if (elapsed_ms % LOG_MS == 0)
            ESP_LOGD(TAG, "waiting BUSY... (%d s)", elapsed_ms / 1000);
        if (elapsed_ms >= TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY timeout after %d s — panel may be disconnected",
                     elapsed_ms / 1000);
            break;
        }
    }
}

/*
 * epd_send_cmd — send a 1-byte command to the panel controller.
 *
 * Sets DC LOW (command mode) then transmits the byte over SPI.
 * CS is managed automatically by the SPI driver.
 *
 * Uses SPI_TRANS_USE_TXDATA so the byte is stored in the transaction
 * descriptor itself (tx_data[0]), bypassing DMA entirely for this
 * single byte and avoiding a bounce-buffer alloc on every command.
 */
static void epd_send_cmd(struct epd_dev_t *dev, uint8_t cmd)
{
    gpio_set_level(EPD_PIN_DC, 0);   /* command mode */
    spi_transaction_t t = {
        .flags   = SPI_TRANS_USE_TXDATA,
        .length  = 8,
        .tx_data = { cmd },
    };
    ESP_ERROR_CHECK(spi_device_transmit(dev->spi_dev, &t));
}

/*
 * epd_send_data — send N bytes of data to the panel controller.
 *
 * Sets DC HIGH (data mode) then transmits in a single SPI transaction.
 * For large transfers (e.g. the init sequence data), keep len ≤ EPD_CHUNK_SIZE.
 */
static void epd_send_data(struct epd_dev_t *dev, const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    gpio_set_level(EPD_PIN_DC, 1);   /* data mode */
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_transmit(dev->spi_dev, &t));
}

/*
 * epd_send_frame — stream the full framebuffer to the panel in chunks.
 *
 * Sends 'len' bytes in EPD_CHUNK_SIZE (5000-byte) segments.  Chunked
 * transfer matches both reference implementations and ensures DMA
 * compatibility.  DC is held HIGH (data mode) for the entire transfer.
 *
 * See TODO.md (backlog) for a note on testing single-shot DMA transfer.
 */
static void epd_send_frame(struct epd_dev_t *dev, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);   /* data mode for entire frame */
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > EPD_CHUNK_SIZE) chunk = EPD_CHUNK_SIZE;
        spi_transaction_t t = {
            .length    = chunk * 8,
            .tx_buffer = data + offset,
        };
        ESP_ERROR_CHECK(spi_device_transmit(dev->spi_dev, &t));
        offset += chunk;
    }
}

/* ── Panel-level operations ──────────────────────────────────────────────── */

static void epd_hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * epd_panel_init — send the full init sequence and wait for POWER_ON.
 *
 * Sequence sourced from Waveshare Jan 2026 display_bsp.cpp and documented
 * in PROGRESS.md.  Register names are per the panel controller datasheet.
 */
static void epd_panel_init(struct epd_dev_t *dev)
{
    /* Panel unlock / identify */
    static const uint8_t d_AA[] = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
    epd_send_cmd(dev, 0xAA); epd_send_data(dev, d_AA, sizeof(d_AA));

    /* Power setting */
    static const uint8_t d_01[] = {0x3F};
    epd_send_cmd(dev, 0x01); epd_send_data(dev, d_01, sizeof(d_01));

    /* Panel setting */
    static const uint8_t d_00[] = {0x5F, 0x69};
    epd_send_cmd(dev, 0x00); epd_send_data(dev, d_00, sizeof(d_00));

    /* Power off sequence setting */
    static const uint8_t d_03[] = {0x00, 0x54, 0x00, 0x44};
    epd_send_cmd(dev, 0x03); epd_send_data(dev, d_03, sizeof(d_03));

    /* Booster soft-start 1 */
    static const uint8_t d_05[] = {0x40, 0x1F, 0x1F, 0x2C};
    epd_send_cmd(dev, 0x05); epd_send_data(dev, d_05, sizeof(d_05));

    /* Booster soft-start 2 */
    static const uint8_t d_06[] = {0x6F, 0x1F, 0x17, 0x49};
    epd_send_cmd(dev, 0x06); epd_send_data(dev, d_06, sizeof(d_06));

    /* Booster soft-start 3 */
    static const uint8_t d_08[] = {0x6F, 0x1F, 0x1F, 0x22};
    epd_send_cmd(dev, 0x08); epd_send_data(dev, d_08, sizeof(d_08));

    /* PLL / frame rate */
    static const uint8_t d_30[] = {0x03};
    epd_send_cmd(dev, 0x30); epd_send_data(dev, d_30, sizeof(d_30));

    /* VCOM and data interval */
    static const uint8_t d_50[] = {0x3F};
    epd_send_cmd(dev, 0x50); epd_send_data(dev, d_50, sizeof(d_50));

    /* TCON */
    static const uint8_t d_60[] = {0x02, 0x00};
    epd_send_cmd(dev, 0x60); epd_send_data(dev, d_60, sizeof(d_60));

    /* Resolution: 0x0320 = 800, 0x01E0 = 480 */
    static const uint8_t d_61[] = {0x03, 0x20, 0x01, 0xE0};
    epd_send_cmd(dev, 0x61); epd_send_data(dev, d_61, sizeof(d_61));

    /* Flash mode */
    static const uint8_t d_84[] = {0x01};
    epd_send_cmd(dev, 0x84); epd_send_data(dev, d_84, sizeof(d_84));

    /* Power saving */
    static const uint8_t d_E3[] = {0x2F};
    epd_send_cmd(dev, 0xE3); epd_send_data(dev, d_E3, sizeof(d_E3));

    /*
     * POWER_ON — starts HV circuits.  This is the last step of init and
     * leaves the panel powered on.  The refresh sequence in epd_display()
     * will issue POWER_ON again (idempotent per the controller state
     * machine) before triggering the refresh, matching the sequence
     * documented in PROGRESS.md and the Waveshare Jan 2026 source.
     */
    epd_send_cmd(dev, 0x04);
    epd_wait_busy();
    ESP_LOGI(TAG, "panel powered on");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t epd_init(epd_handle_t *out)
{
    struct epd_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;

    /* Configure DC and RST as outputs, BUSY as input with pull-up */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,  /* float-high when panel absent */
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* Safe initial state before reset */
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_DC,  0);

    /*
     * SPI bus.
     *
     * max_transfer_sz is set to EPD_CHUNK_SIZE (5000 bytes) — the largest
     * single transaction we issue.  This is intentionally the exact chunk
     * size, not larger.  Callers must not pass more than EPD_CHUNK_SIZE
     * bytes to epd_send_data() in a single call; epd_send_frame() enforces
     * this by chunking the framebuffer internally.
     *
     * Note: 5000 > SPI_MAX_DMA_LEN (4092 bytes, one DMA descriptor).  The
     * IDF driver handles this via a linked descriptor chain, which is fully
     * supported on ESP32-S3.
     */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = EPD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = EPD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EPD_CHUNK_SIZE,
    };
    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }

    /* SPI device — 40 MHz half-duplex, CS on EPD_PIN_CS */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = EPD_SPI_FREQ_HZ,
        .mode           = 0,              /* CPOL=0, CPHA=0 */
        .spics_io_num   = EPD_PIN_CS,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &dev->spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(EPD_SPI_HOST);
        free(dev);
        return err;
    }

    epd_hw_reset();
    epd_panel_init(dev);

    ESP_LOGI(TAG, "init OK");
    *out = dev;
    return ESP_OK;
}

esp_err_t epd_display(epd_handle_t h, const uint8_t *framebuf, size_t len)
{
    if (!h || !framebuf) return ESP_ERR_INVALID_ARG;
    if (len != EPD_FB_SIZE) {
        ESP_LOGE(TAG, "framebuf len %zu != EPD_FB_SIZE %u", len, EPD_FB_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "writing frame (%u bytes in %u-byte chunks)...",
             EPD_FB_SIZE, EPD_CHUNK_SIZE);

    /* Write pixel data to panel SRAM */
    epd_send_cmd(h, 0x10);
    epd_send_frame(h, framebuf, len);

    /*
     * Refresh sequence: POWER_ON → boost → DISPLAY_REFRESH → POWER_OFF.
     *
     * The POWER_ON here is intentional even though init already issued one.
     * The controller requires POWER_ON at the start of every refresh cycle
     * per the sequence documented in PROGRESS.md (Waveshare Jan 2026 source).
     * Sending POWER_ON when already on is idempotent; after a second or
     * subsequent call, the preceding epd_display() will have ended with
     * POWER_OFF, making this POWER_ON strictly necessary.
     */
    static const uint8_t d_06[] = {0x6F, 0x1F, 0x17, 0x49};
    static const uint8_t d_12[] = {0x00};
    static const uint8_t d_02[] = {0x00};

    epd_send_cmd(h, 0x04);                              /* POWER_ON */
    epd_wait_busy();
    epd_send_cmd(h, 0x06); epd_send_data(h, d_06, sizeof(d_06)); /* booster */
    epd_send_cmd(h, 0x12); epd_send_data(h, d_12, sizeof(d_12)); /* DISPLAY_REFRESH */
    epd_wait_busy();                                    /* ~30 s */
    epd_send_cmd(h, 0x02); epd_send_data(h, d_02, sizeof(d_02)); /* POWER_OFF */
    epd_wait_busy();

    ESP_LOGI(TAG, "refresh complete");
    return ESP_OK;
}

esp_err_t epd_sleep(epd_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;

    /*
     * POWER_OFF first — safe even if already off after epd_display().
     * DEEP_SLEEP check code 0xA5 prevents accidental sleep entry.
     */
    static const uint8_t d_02[] = {0x00};
    epd_send_cmd(h, 0x02); epd_send_data(h, d_02, sizeof(d_02)); /* POWER_OFF */
    epd_wait_busy();

    static const uint8_t d_07[] = {0xA5};
    epd_send_cmd(h, 0x07); epd_send_data(h, d_07, sizeof(d_07)); /* DEEP_SLEEP */

    ESP_LOGI(TAG, "panel sleeping");
    return ESP_OK;
}

void epd_deinit(epd_handle_t h)
{
    if (!h) return;
    spi_bus_remove_device(h->spi_dev);
    spi_bus_free(EPD_SPI_HOST);
    free(h);
}

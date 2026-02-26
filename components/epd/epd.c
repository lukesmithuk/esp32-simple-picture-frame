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
static void epd_wait_busy(struct epd_dev_t *dev)
{
    const int POLL_MS    = 10;
    const int LOG_MS     = 5000;
    const int TIMEOUT_MS = 400000;
    int elapsed_ms = 0;

    while (1) {
        /* HIGH = IDLE, LOW = BUSY */
        if (gpio_get_level(EPD_PIN_BUSY) != 0) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        elapsed_ms += POLL_MS;

        if (elapsed_ms % LOG_MS == 0) {
            ESP_LOGI(TAG, "waiting BUSY... (%d s)", elapsed_ms / 1000);
        }

        if (elapsed_ms >= TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY timeout after %d s", elapsed_ms / 1000);
            break;
        }
    }
}

/*
 * epd_send_byte — low-level polling transmit of a single byte.
 */
static void epd_send_byte(struct epd_dev_t *dev, uint8_t data)
{
    spi_transaction_t t = {};
    t.length  = 8;
    t.tx_buffer = &data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(dev->spi_dev, &t));
}

/*
 * epd_send_cmd — send a 1-byte command.
 * Toggles CS for the single byte, matching aitjcize reference.
 */
static void epd_send_cmd(struct epd_dev_t *dev, uint8_t cmd)
{
    gpio_set_level(EPD_PIN_DC, 0); // DC low = command
    gpio_set_level(EPD_PIN_CS, 0);
    epd_send_byte(dev, cmd);
    gpio_set_level(EPD_PIN_CS, 1);
}

/*
 * epd_send_cmd_data — send a command followed by N bytes of data.
 * Toggles CS for EVERY byte, matching aitjcize reference.
 */
static void epd_send_cmd_data(struct epd_dev_t *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
    epd_send_cmd(dev, cmd);
    for (size_t i = 0; i < len; i++) {
        gpio_set_level(EPD_PIN_DC, 1); // DC high = data
        gpio_set_level(EPD_PIN_CS, 0);
        epd_send_byte(dev, data[i]);
        gpio_set_level(EPD_PIN_CS, 1);
    }
}

/*
 * epd_send_frame — stream the full framebuffer to the panel.
 * Holds CS LOW for the entire 192KB transfer.
 */
static void epd_send_frame(struct epd_dev_t *dev, const uint8_t *data, size_t len)
{
    /* Send the 0x10 command separately with its own CS cycle */
    epd_send_cmd(dev, 0x10);

    /* Stream the frame data in one long CS LOW cycle */
    gpio_set_level(EPD_PIN_DC, 1); // DC high = data
    gpio_set_level(EPD_PIN_CS, 0);
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > EPD_CHUNK_SIZE) chunk = EPD_CHUNK_SIZE;
        spi_transaction_t t = {
            .length    = chunk * 8,
            .tx_buffer = data + offset,
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(dev->spi_dev, &t));
        offset += chunk;
    }
    gpio_set_level(EPD_PIN_CS, 1);

    epd_send_cmd(dev, 0x04);
    epd_wait_busy(dev);
    static const uint8_t d_12[] = {0x00};
    epd_send_cmd_data(dev, 0x12, d_12, sizeof(d_12));
    epd_wait_busy(dev);
    static const uint8_t d_02[] = {0x00};
    epd_send_cmd_data(dev, 0x02, d_02, sizeof(d_02));
    epd_wait_busy(dev);
}

/* ── Panel-level operations ──────────────────────────────────────────────── */

static void epd_gpio_init(struct epd_dev_t *dev)
{
    /* 
     * Explicitly reset pins to clear any peripheral functions (like JTAG).
     * GPIO 10-13 are shared with JTAG on ESP32-S3.
     */
    gpio_reset_pin(EPD_PIN_DC);
    gpio_reset_pin(EPD_PIN_RST);
    gpio_reset_pin(EPD_PIN_CS);
    gpio_reset_pin(EPD_PIN_BUSY);
    

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST)
                      | (1ULL << EPD_PIN_CS),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
}

static esp_err_t epd_spi_init(struct epd_dev_t *dev)
{
    gpio_reset_pin(EPD_PIN_MOSI);
    gpio_reset_pin(EPD_PIN_CLK);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = EPD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = EPD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 1200 * 825 / 2 + 100, //EPD_CHUNK_SIZE,
    };
    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = EPD_SPI_FREQ_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 7,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &dev->spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(EPD_SPI_HOST);
        return err;
    }

    return ESP_OK;
}

static void epd_hw_reset(void)
{
    ESP_LOGI(TAG, "hardware reset...");
    gpio_set_level(EPD_PIN_RST, 1); 
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); 
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); 
    vTaskDelay(pdMS_TO_TICKS(50));

    if (gpio_get_level(EPD_PIN_BUSY) == 1) {
        ESP_LOGW(TAG, "BUSY pin high after reset — panel may not be responding");
    } else {
        ESP_LOGI(TAG, "BUSY pin low — panel is initialising");
    }

    epd_wait_busy(NULL);
}

/*
 * epd_panel_init — send the full init sequence and wait for POWER_ON.
 *
 * Sequence updated to match verified Spectra 6 (7IN3E) reference driver.
 * All commands sent with CS held LOW for their respective data phases.
 */
static void epd_panel_init(struct epd_dev_t *dev)
{
    epd_wait_busy(dev);

    /* Panel unlock / identify */
    static const uint8_t d_AA[] = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
    epd_send_cmd_data(dev, 0xAA, d_AA, sizeof(d_AA));

    /* Power setting (0x01) */
    static const uint8_t d_01[] = {0x3F, 0x00, 0x32, 0x2A, 0x0E, 0x2A};
    epd_send_cmd_data(dev, 0x01, d_01, sizeof(d_01));

    /* Panel setting (0x00) */
    static const uint8_t d_00[] = {0x5F, 0x69};
    epd_send_cmd_data(dev, 0x00, d_00, sizeof(d_00));

    /* Power off sequence setting (0x03) */
    static const uint8_t d_03[] = {0x00, 0x54, 0x00, 0x44};
    epd_send_cmd_data(dev, 0x03, d_03, sizeof(d_03));

    /* Booster soft-start 1, 2, 3 (0x05, 0x06, 0x08) */
    static const uint8_t d_05[] = {0x40, 0x1F, 0x1F, 0x2C};
    epd_send_cmd_data(dev, 0x05, d_05, sizeof(d_05));

    static const uint8_t d_06[] = {0x6F, 0x1F, 0x16, 0x25};
    epd_send_cmd_data(dev, 0x06, d_06, sizeof(d_06));

    static const uint8_t d_08[] = {0x6F, 0x1F, 0x1F, 0x22};
    epd_send_cmd_data(dev, 0x08, d_08, sizeof(d_08));

    /* IPC (0x13) Internal Power Control */
    static const uint8_t d_13[] = {0x00, 0x04};
    epd_send_cmd_data(dev, 0x13, d_13, sizeof(d_13));

    /* PLL / frame rate (0x30) */
    static const uint8_t d_30[] = {0x02};
    epd_send_cmd_data(dev, 0x30, d_30, sizeof(d_30));

    /* TSE (0x41) Temperature Sensor Enable */
    static const uint8_t d_41[] = {0x00};
    epd_send_cmd_data(dev, 0x41, d_41, sizeof(d_41));

    /* VCOM and data interval (0x50) */
    static const uint8_t d_50[] = {0x3F};
    epd_send_cmd_data(dev, 0x50, d_50, sizeof(d_50));

    /* TCON (0x60) */
    static const uint8_t d_60[] = {0x02, 0x00};
    epd_send_cmd_data(dev, 0x60, d_60, sizeof(d_60));

    /* Resolution (0x61) */
    static const uint8_t d_61[] = {0x03, 0x20, 0x01, 0xE0};
    epd_send_cmd_data(dev, 0x61, d_61, sizeof(d_61));

    /* VDCS (0x82) VCOM DC Setting */
    static const uint8_t d_82[] = {0x1E};
    epd_send_cmd_data(dev, 0x82, d_82, sizeof(d_82));

    /* T_VDCS (0x84) - Temperature VCOM DC Setting */
    static const uint8_t d_84[] = {0x01};
    epd_send_cmd_data(dev, 0x84, d_84, sizeof(d_84));

    /* AGID (0x86) */
    static const uint8_t d_86[] = {0x00};
    epd_send_cmd_data(dev, 0x86, d_86, sizeof(d_86));

    /* Power saving (0xE3) */
    static const uint8_t d_E3[] = {0x2F};
    epd_send_cmd_data(dev, 0xE3, d_E3, sizeof(d_E3));

    /* CCSET (0xE0) - Color Control Setting */
    static const uint8_t d_E0[] = {0x00};
    epd_send_cmd_data(dev, 0xE0, d_E0, sizeof(d_E0));

    /* TSSET (0xE6) - Temperature Sensor Setting */
    static const uint8_t d_E6[] = {0x00};
    epd_send_cmd_data(dev, 0xE6, d_E6, sizeof(d_E6));

    /* POWER_ON (0x04) — starts HV circuits */
    epd_send_cmd(dev, 0x04);
    epd_wait_busy(dev);
    ESP_LOGI(TAG, "panel initialized and powered on");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t epd_init(epd_handle_t *out)
{
    esp_err_t err = ESP_OK;
    struct epd_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;

    epd_gpio_init(dev);
    err = epd_spi_init(dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }
    epd_hw_reset();
    epd_wait_busy(dev);
    vTaskDelay(pdMS_TO_TICKS(50));

    epd_panel_init(dev);

    ESP_LOGI(TAG, "init OK");
    *out = dev;
    return err;
}

esp_err_t epd_display(epd_handle_t h, const uint8_t *framebuf, size_t len)
{
    if (!h || !framebuf) return ESP_ERR_INVALID_ARG;
    if (len != EPD_FB_SIZE) {
        ESP_LOGE(TAG, "framebuf len %zu != EPD_FB_SIZE %u", len, EPD_FB_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "writing frame (%u bytes)...", EPD_FB_SIZE);

    /* Write pixel data to panel SRAM (0x10 + frame data) */
    epd_send_frame(h, framebuf, len);

    /*
     * Refresh sequence: DISPLAY_REFRESH (0x12) → wait (12-15s) → POWER_OFF (0x02).
     */
    epd_send_cmd(h, 0x12);
    epd_wait_busy(h);

    epd_send_cmd(h, 0x02);
    epd_wait_busy(h);

    ESP_LOGI(TAG, "refresh complete");
    return ESP_OK;
}

esp_err_t epd_sleep(epd_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;

    /*
     * POWER_OFF (0x02) first — safe even if already off.
     * DEEP_SLEEP check code 0xA5 (0x07) prevents accidental sleep entry.
     */
    static const uint8_t d_02[] = {0x00};
    epd_send_cmd_data(h, 0x02, d_02, sizeof(d_02));
    epd_wait_busy(h);

    static const uint8_t d_07[] = {0xA5};
    epd_send_cmd_data(h, 0x07, d_07, sizeof(d_07));

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

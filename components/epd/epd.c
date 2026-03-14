#include "epd.h"

#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "epd";

/* ── Pin assignments (Waveshare ESP32-S3-PhotoPainter, confirmed from schematic) */
#define EPD_PIN_DC    GPIO_NUM_8
#define EPD_PIN_CS    GPIO_NUM_9
#define EPD_PIN_SCK   GPIO_NUM_10
#define EPD_PIN_MOSI  GPIO_NUM_11
#define EPD_PIN_RST   GPIO_NUM_12
#define EPD_PIN_BUSY  GPIO_NUM_13
#define EPD_SPI_HOST  SPI2_HOST

/* ── SPI constraints */
#define SPI_MAX_CHUNK    4092   /* max bytes per spi_device_polling_start call */
#define DATA_CHUNK_SIZE  128    /* per-CS-window chunk for frame buffer transfer */

static spi_device_handle_t s_spi = NULL;

/* ── Low-level SPI helpers ──────────────────────────────────────────────── */

static void spi_bus_begin(void)
{
    esp_err_t ret = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    assert(ret == ESP_OK);
}

static void spi_bus_end(void)
{
    spi_device_release_bus(s_spi);
}

static void spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {};
    t.rxlength = 0;
    while (len > 0) {
        size_t chunk = (len > SPI_MAX_CHUNK) ? SPI_MAX_CHUNK : len;
        t.length    = chunk * 8;
        t.tx_buffer = data;
        esp_err_t ret = spi_device_polling_start(s_spi, &t, portMAX_DELAY);
        if (ret == ESP_OK) {
            ret = spi_device_polling_end(s_spi, portMAX_DELAY);
        }
        assert(ret == ESP_OK);
        data += chunk;
        len  -= chunk;
    }
}

/* ── Display protocol helpers ───────────────────────────────────────────── */

/**
 * Send a command byte followed by optional data bytes, all in a single
 * CS-low window.  Data must fit in a 16-byte stack buffer (panel protocol
 * limits data to ≤6 bytes per command).
 */
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 0);   /* DC low = command */
    spi_bus_begin();
    gpio_set_level(EPD_PIN_CS, 0);   /* CS low */

    spi_transaction_ext_t cmd_t = {
        .command_bits = 8,
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD,
            .cmd   = cmd,
        },
    };
    esp_err_t ret = spi_device_polling_start(s_spi, &cmd_t.base, portMAX_DELAY);
    if (ret == ESP_OK) {
        ret = spi_device_polling_end(s_spi, portMAX_DELAY);
    }
    assert(ret == ESP_OK);

    if (len > 0) {
        gpio_set_level(EPD_PIN_DC, 1);  /* DC high = data */
        uint8_t buf[16];
        assert(len <= sizeof(buf));
        memcpy(buf, data, len);
        spi_write(buf, len);
    }

    gpio_set_level(EPD_PIN_CS, 1);   /* CS high */
    spi_bus_end();
}

static void send_command(uint8_t cmd)
{
    cmd_data(cmd, NULL, 0);
}

/**
 * Transfer the full frame buffer in DATA_CHUNK_SIZE-byte CS windows.
 * Each chunk is copied to a stack-local buffer to avoid PSRAM→DMA issues.
 */
static void send_buffer(const uint8_t *data, int len)
{
    uint8_t buf[DATA_CHUNK_SIZE];
    const uint8_t *ptr = data;
    int remaining = len;

    ESP_LOGI(TAG, "Sending %d bytes in %d-byte chunks", len, DATA_CHUNK_SIZE);

    while (remaining > 0) {
        int chunk = (remaining > DATA_CHUNK_SIZE) ? DATA_CHUNK_SIZE : remaining;

        memcpy(buf, ptr, chunk);  /* copy to internal RAM for DMA */

        gpio_set_level(EPD_PIN_DC, 1);  /* DC high = data */
        spi_bus_begin();
        gpio_set_level(EPD_PIN_CS, 0);
        spi_write(buf, chunk);
        gpio_set_level(EPD_PIN_CS, 1);
        spi_bus_end();

        ptr       += chunk;
        remaining -= chunk;
    }

    ESP_LOGI(TAG, "Buffer send complete");
}

static bool is_busy(void)
{
    return gpio_get_level(EPD_PIN_BUSY) == 0;  /* active LOW */
}

static void wait_busy(const char *label)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    int wait_count = 0;
    while (is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++wait_count > 4000) {  /* 40 s timeout */
            ESP_LOGW(TAG, "[%s] BUSY timeout after 40s", label);
            return;
        }
    }
    ESP_LOGD(TAG, "[%s] BUSY cleared after %d ms", label, wait_count * 10);
}

/* ── Hardware setup ─────────────────────────────────────────────────────── */

static void epd_gpio_init(void)
{
    /* Set output levels BEFORE enabling output drivers to avoid glitches */
    gpio_set_level(EPD_PIN_CS,  1);  /* CS HIGH = deselected */
    gpio_set_level(EPD_PIN_DC,  0);  /* DC LOW  = command mode */
    gpio_set_level(EPD_PIN_RST, 1);  /* RST HIGH = not in reset */

    gpio_config_t out_conf = {
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_CS),
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));

    gpio_config_t in_conf = {
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
    };
    ESP_ERROR_CHECK(gpio_config(&in_conf));
}

static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── Panel init sequence ────────────────────────────────────────────────── */

static void send_init_sequence(void)
{
    cmd_data(0xAA, (uint8_t[]){0x49, 0x55, 0x20, 0x08, 0x09, 0x18}, 6);  /* CMDH  */
    cmd_data(0x01, (uint8_t[]){0x3F},                                  1);  /* PWRR  */
    cmd_data(0x00, (uint8_t[]){0x5F, 0x69},                            2);  /* PSR   */
    cmd_data(0x03, (uint8_t[]){0x00, 0x54, 0x00, 0x44},                4);  /* POFS  */
    cmd_data(0x05, (uint8_t[]){0x40, 0x1F, 0x1F, 0x2C},                4);  /* BTST1 */
    cmd_data(0x06, (uint8_t[]){0x6F, 0x1F, 0x17, 0x49},                4);  /* BTST2 */
    cmd_data(0x08, (uint8_t[]){0x6F, 0x1F, 0x1F, 0x22},                4);  /* BTST3 */
    cmd_data(0x30, (uint8_t[]){0x03},                                  1);  /* PLL   */
    cmd_data(0x50, (uint8_t[]){0x3F},                                  1);  /* CDI   */
    cmd_data(0x60, (uint8_t[]){0x02, 0x00},                            2);  /* TCON  */
    cmd_data(0x61, (uint8_t[]){0x03, 0x20, 0x01, 0xE0},                4);  /* TRES  */
    cmd_data(0x84, (uint8_t[]){0x01},                                  1);  /* T_VDCS*/
    cmd_data(0xE3, (uint8_t[]){0x2F},                                  1);  /* PWS   */
}

/* ── Display update cycle ───────────────────────────────────────────────── */

/**
 * Full panel cycle (matches aitjcize driver_ed2208_gca.c exactly):
 *   hw_reset → init → wait_busy("init")
 *   → 0x10 + data → wait_busy("data")    ← critical: wait BEFORE PON
 *   → 0x04 PON → wait_busy("power_on")
 *   → 0x12{00} DRF → wait_busy("refresh")
 *   → 0x02{00} POF → wait_busy("power_off")
 *   → 0x07{A5} DSLP
 */
static esp_err_t display_update_cycle(const uint8_t *image)
{
    hw_reset();
    wait_busy("reset");

    send_init_sequence();
    wait_busy("init");

    send_command(0x10);                           /* DATA_START_TRANSMISSION */
    send_buffer(image, EPD_BUF_SIZE);
    wait_busy("data");                            /* must wait before PON */

    send_command(0x04);                           /* POWER_ON */
    wait_busy("power_on");

    cmd_data(0x12, (uint8_t[]){0x00}, 1);         /* DISPLAY_REFRESH */
    wait_busy("refresh");

    cmd_data(0x02, (uint8_t[]){0x00}, 1);         /* POWER_OFF */
    wait_busy("power_off");

    cmd_data(0x07, (uint8_t[]){0xA5}, 1);         /* DEEP_SLEEP */

    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t epd_init(void)
{
    ESP_LOGI(TAG, "EPD init");

    spi_bus_config_t buscfg = {
        .mosi_io_num     = EPD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = EPD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 1200 * 825 / 2 + 100,
    };
    esp_err_t ret = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = -1,   /* CS manually controlled */
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(EPD_SPI_HOST);
        return ret;
    }

    epd_gpio_init();

    ESP_LOGI(TAG, "EPD init complete");
    return ESP_OK;
}

/**
 * Rotate the frame buffer 180° in-place.
 * The panel scans bottom-right to top-left relative to the logical
 * coordinate system, so we reverse byte order and swap nibbles.
 */
static void rotate_180(uint8_t *buf, size_t len)
{
    uint8_t *lo = buf;
    uint8_t *hi = buf + len - 1;
    while (lo < hi) {
        uint8_t a = *lo, b = *hi;
        *lo++ = (b >> 4) | (b << 4);
        *hi-- = (a >> 4) | (a << 4);
    }
    if (lo == hi) {
        *lo = (*lo >> 4) | (*lo << 4);
    }
}

esp_err_t epd_display(const uint8_t *frame_buf)
{
    ESP_LOGI(TAG, "Starting display update (%d bytes)", EPD_BUF_SIZE);

    /* Rotate 180° into a temporary copy — the caller's buffer is const. */
    uint8_t *rotated = heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!rotated) {
        ESP_LOGE(TAG, "Failed to alloc rotation buffer");
        return ESP_ERR_NO_MEM;
    }
    memcpy(rotated, frame_buf, EPD_BUF_SIZE);
    rotate_180(rotated, EPD_BUF_SIZE);

    esp_err_t ret = display_update_cycle(rotated);
    free(rotated);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Display update complete");
    }
    return ret;
}

void epd_deinit(void)
{
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        spi_bus_free(EPD_SPI_HOST);
    }
    ESP_LOGI(TAG, "EPD deinit complete");
}

uint8_t *epd_alloc_frame_buf(void)
{
    return (uint8_t *) heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
}

void epd_fill_color(uint8_t *frame_buf, epd_color_t color)
{
    uint8_t packed = ((uint8_t)color << 4) | (uint8_t)color;
    memset(frame_buf, packed, EPD_BUF_SIZE);
}

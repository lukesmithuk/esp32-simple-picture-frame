#include "board.h"

#include "axp2101.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "pcf85063.h"

static const char *TAG = "board";

/* ── Pin assignments ──────────────────────────────────────────────────── */
#define BOARD_I2C_SDA GPIO_NUM_47
#define BOARD_I2C_SCL GPIO_NUM_48

static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* ── I2C bus recovery ─────────────────────────────────────────────────── */

/**
 * Toggle SCL 9 times then send a STOP condition to unstick any device that
 * was mid-transaction when the previous boot ended (e.g. interrupted by
 * deep sleep or a crash).  Must be called before i2c_new_master_bus().
 */
static void i2c_bus_recover(void)
{
    /* Briefly drive both lines as open-drain outputs */
    gpio_config_t cfg = {
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << BOARD_I2C_SDA) | (1ULL << BOARD_I2C_SCL),
    };
    gpio_config(&cfg);

    gpio_set_level(BOARD_I2C_SDA, 1);
    gpio_set_level(BOARD_I2C_SCL, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* Toggle SCL 9 times while SDA is high */
    for (int i = 0; i < 9; i++) {
        gpio_set_level(BOARD_I2C_SCL, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(BOARD_I2C_SCL, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Send STOP: SDA low → SCL high → SDA high */
    gpio_set_level(BOARD_I2C_SDA, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(BOARD_I2C_SCL, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(BOARD_I2C_SDA, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    ESP_LOGI(TAG, "I2C bus recovery complete");
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Board init start");

    i2c_bus_recover();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port        = I2C_NUM_0,
        .sda_io_num      = BOARD_I2C_SDA,
        .scl_io_num      = BOARD_I2C_SCL,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    axp2101_init(s_i2c_bus);
    axp2101_cmd_init();

    ret = pcf85063_init(s_i2c_bus);
    if (ret != ESP_OK) {
        /* RTC is non-fatal — log and continue */
        ESP_LOGW(TAG, "PCF85063 init failed (non-fatal): %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Board init complete");
    return ESP_OK;
}

esp_err_t board_epd_power(bool on)
{
    axp2101_epd_power(on);
    if (on) {
        /* Allow ALDO3 rail to stabilise before SPI transactions */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t board_sleep(void)
{
    ESP_LOGW(TAG, "Entering PMIC sleep — serial output will stop");
    axp2101_basic_sleep_start();
    return ESP_OK;
}

bool board_rtc_is_available(void)
{
    return pcf85063_is_available();
}

esp_err_t board_rtc_get_time(time_t *t)
{
    return pcf85063_read_time(t);
}

esp_err_t board_rtc_set_time(time_t t)
{
    return pcf85063_write_time(t);
}

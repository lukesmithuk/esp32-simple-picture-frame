#include "board.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "axp2101.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf85063.h"

static const char *TAG = "board";

/* ── Pin assignments ──────────────────────────────────────────────────── */
#define BOARD_I2C_SDA    GPIO_NUM_47
#define BOARD_I2C_SCL    GPIO_NUM_48
#define BOARD_RTC_INT    GPIO_NUM_6

/* ── I2C bus init ────────────────────────────────────────────────────── */

/**
 * Configure GPIOs for bit-banged I2C, then toggle SCL 9 times + STOP to
 * unstick any slave that was mid-transaction when the previous boot ended
 * (e.g. interrupted by deep sleep or a crash).
 */
static void i2c_bus_init(void)
{
    gpio_reset_pin(BOARD_I2C_SDA);
    gpio_reset_pin(BOARD_I2C_SCL);

    /* SDA: open-drain — our '1' just releases the line.
     * SCL: push-pull  — force SCL high even against clock-stretching. */
    gpio_config_t sda_cfg = {
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << BOARD_I2C_SDA),
    };
    gpio_config_t scl_cfg = {
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << BOARD_I2C_SCL),
    };
    gpio_config(&sda_cfg);
    gpio_config(&scl_cfg);

    gpio_set_level(BOARD_I2C_SDA, 1);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(50);

    /* Toggle SCL 9 times to free any slave holding SDA (NXP AN10216). */
    for (int i = 0; i < 9; i++) {
        gpio_set_level(BOARD_I2C_SCL, 0);
        esp_rom_delay_us(10);
        gpio_set_level(BOARD_I2C_SCL, 1);
        esp_rom_delay_us(10);
    }

    /* STOP condition: SCL low → SDA low → SCL high → SDA high */
    gpio_set_level(BOARD_I2C_SCL, 0);
    esp_rom_delay_us(10);
    gpio_set_level(BOARD_I2C_SDA, 0);
    esp_rom_delay_us(10);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(10);
    gpio_set_level(BOARD_I2C_SDA, 1);
    esp_rom_delay_us(10);

    ESP_LOGI(TAG, "I2C bus init complete (SCL=%d SDA=%d)",
             gpio_get_level(BOARD_I2C_SCL), gpio_get_level(BOARD_I2C_SDA));
}

/* ── Bit-banged I2C ──────────────────────────────────────────────────── */

/*
 * Bit-banged I2C master (~100 kHz) used for ALL I2C on this board.
 *
 * The IDF v5.5.3 I2C driver on ESP32-S3 fires corrupt SCL clear-bus pulses
 * on any hardware timeout, permanently wedging the PMIC after RTS reset.
 * Bit-banging bypasses the IDF driver entirely.
 *
 * Requires GPIOs already configured as:
 *   SDA = INPUT_OUTPUT_OD + pullup   (set by i2c_bus_init)
 *   SCL = INPUT_OUTPUT   + pullup
 */

#define BB_HALF_PERIOD_US  5   /* ~100 kHz */

static void bb_start(void)
{
    gpio_set_level(BOARD_I2C_SDA, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SDA, 0);   /* SDA ↓ while SCL high = START */
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 0);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
}

static void bb_stop(void)
{
    gpio_set_level(BOARD_I2C_SDA, 0);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SDA, 1);   /* SDA ↑ while SCL high = STOP */
    esp_rom_delay_us(BB_HALF_PERIOD_US);
}

static bool bb_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(BOARD_I2C_SDA, (byte >> i) & 1);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
        gpio_set_level(BOARD_I2C_SCL, 1);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
        gpio_set_level(BOARD_I2C_SCL, 0);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
    }
    /* Release SDA, clock in ACK */
    gpio_set_level(BOARD_I2C_SDA, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    bool ack = (gpio_get_level(BOARD_I2C_SDA) == 0);
    gpio_set_level(BOARD_I2C_SCL, 0);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    return ack;
}

static uint8_t bb_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    gpio_set_level(BOARD_I2C_SDA, 1);   /* release for reading */
    for (int i = 7; i >= 0; i--) {
        esp_rom_delay_us(BB_HALF_PERIOD_US);
        gpio_set_level(BOARD_I2C_SCL, 1);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
        if (gpio_get_level(BOARD_I2C_SDA))
            byte |= (1 << i);
        gpio_set_level(BOARD_I2C_SCL, 0);
    }
    /* Send ACK (low) or NACK (high) */
    gpio_set_level(BOARD_I2C_SDA, send_ack ? 0 : 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SCL, 0);
    esp_rom_delay_us(BB_HALF_PERIOD_US);
    gpio_set_level(BOARD_I2C_SDA, 1);   /* release */
    return byte;
}

int board_bb_i2c_read(uint8_t dev_addr, uint8_t reg_addr,
                      uint8_t *data, uint8_t len)
{
    bb_start();
    if (!bb_write_byte(dev_addr << 1)) { bb_stop(); return -1; }
    if (!bb_write_byte(reg_addr))       { bb_stop(); return -1; }
    bb_start();  /* repeated start */
    if (!bb_write_byte((dev_addr << 1) | 1)) { bb_stop(); return -1; }
    for (int i = 0; i < len; i++)
        data[i] = bb_read_byte(i < len - 1);   /* ACK all except last */
    bb_stop();
    return 0;
}

int board_bb_i2c_write(uint8_t dev_addr, uint8_t reg_addr,
                       const uint8_t *data, uint8_t len)
{
    bb_start();
    if (!bb_write_byte(dev_addr << 1)) { bb_stop(); return -1; }
    if (!bb_write_byte(reg_addr))       { bb_stop(); return -1; }
    for (int i = 0; i < len; i++) {
        if (!bb_write_byte(data[i]))    { bb_stop(); return -1; }
    }
    bb_stop();
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

/**
 * Set RTC to compile time if the oscillator-stop flag is set (clock was
 * never set or lost power). Gives a rough starting point for alarms.
 */
static void rtc_set_compile_time_if_needed(void)
{
    time_t t;
    if (pcf85063_read_time(&t) != ESP_ERR_INVALID_STATE)
        return;  /* OSF not set — clock is running */

    /* Parse __DATE__ "Mar 11 2026" and __TIME__ "20:41:28" */
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    char mon_str[4];
    int day, year, hour, min, sec;
    sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);

    int mon = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon_str, months[i]) == 0) { mon = i; break; }
    }

    struct tm tm = {
        .tm_sec  = sec,
        .tm_min  = min,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon  = mon,
        .tm_year = year - 1900,
    };
    time_t compile_time = mktime(&tm);

    if (pcf85063_write_time(compile_time) == ESP_OK) {
        ESP_LOGW(TAG, "RTC was unset — initialized to compile time");
    }
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Board init start");

    i2c_bus_init();

    esp_err_t ret = axp2101_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMIC not found");
        return ret;
    }
    ret = axp2101_cmd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMIC config failed");
        return ret;
    }

    ret = pcf85063_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 init failed (non-fatal): %s", esp_err_to_name(ret));
    } else {
        rtc_set_compile_time_if_needed();
    }

    ESP_LOGI(TAG, "Board init complete");
    return ESP_OK;
}

esp_err_t board_epd_power(bool on)
{
    esp_err_t ret = axp2101_epd_power(on);
    if (ret != ESP_OK) {
        return ret;
    }
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

esp_err_t board_rtc_set_alarm(int hour, int minute, int second)
{
    return pcf85063_set_alarm(hour, minute, second);
}

esp_err_t board_rtc_clear_alarm_flag(void)
{
    return pcf85063_clear_alarm_flag();
}

void board_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Configuring EXT0 wakeup on GPIO%d (active LOW)", BOARD_RTC_INT);
    esp_err_t ret = esp_sleep_enable_ext0_wakeup(BOARD_RTC_INT, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable EXT0 wakeup: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

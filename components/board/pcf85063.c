#include "pcf85063.h"
#include "board.h"

#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "pcf85063_rtc";

#define PCF85063_ADDR 0x51

// PCF85063ATL Register addresses
#define PCF85063_ADDR_CONTROL_1 0x00
#define PCF85063_ADDR_CONTROL_2 0x01
#define PCF85063_ADDR_OFFSET 0x02
#define PCF85063_ADDR_RAM_BYTE 0x03
#define PCF85063_ADDR_SECONDS 0x04
#define PCF85063_ADDR_MINUTES 0x05
#define PCF85063_ADDR_HOURS 0x06
#define PCF85063_ADDR_DAYS 0x07
#define PCF85063_ADDR_WEEKDAYS 0x08
#define PCF85063_ADDR_MONTHS 0x09
#define PCF85063_ADDR_YEARS 0x0A

// Alarm registers
#define PCF85063_ADDR_SECOND_ALARM  0x0B
#define PCF85063_ADDR_MINUTE_ALARM  0x0C
#define PCF85063_ADDR_HOUR_ALARM    0x0D
#define PCF85063_ADDR_DAY_ALARM     0x0E
#define PCF85063_ADDR_WEEKDAY_ALARM 0x0F

// Bit masks
#define PCF85063_STOP_BIT    0x20  // STOP bit in Control_1 register
#define PCF85063_CAP_SEL_BIT 0x01  // Capacitor selection bit
#define PCF85063_OSF_BIT     0x80  // Oscillator stop flag (in seconds register)
#define PCF85063_AEN_BIT     0x80  // Alarm enable (bit 7 of alarm regs, 1=disabled)
#define PCF85063_AF_BIT      0x40  // Alarm flag in Control_2
#define PCF85063_AIE_BIT     0x80  // Alarm interrupt enable in Control_2

static bool rtc_initialized = false;
static bool rtc_available = false;

// BCD conversion helpers
static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return ((dec / 10) << 4) | (dec % 10);
}

esp_err_t pcf85063_init(void)
{

    ESP_LOGI(TAG, "Initializing PCF85063ATL RTC");

    uint8_t data;
    if (board_bb_i2c_read(PCF85063_ADDR, PCF85063_ADDR_CONTROL_1, &data, 1) != 0) {
        ESP_LOGE(TAG, "Failed to communicate with PCF85063ATL");
        rtc_available = false;
        rtc_initialized = true;
        return ESP_ERR_NOT_FOUND;
    }

    // Clear STOP bit if set (enable counting) and set CAP_SEL for 7pF
    data &= ~PCF85063_STOP_BIT;
    data |= PCF85063_CAP_SEL_BIT;
    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_CONTROL_1, &data, 1) != 0) {
        ESP_LOGE(TAG, "Failed to configure PCF85063ATL");
        rtc_available = false;
        rtc_initialized = true;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PCF85063ATL RTC initialized successfully");
    rtc_available = true;
    rtc_initialized = true;
    return ESP_OK;
}

esp_err_t pcf85063_read_time(time_t *time_out)
{
    if (!rtc_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!rtc_available)
        return ESP_ERR_NOT_FOUND;

    uint8_t data[7];
    if (board_bb_i2c_read(PCF85063_ADDR, PCF85063_ADDR_SECONDS, data, 7) != 0) {
        ESP_LOGE(TAG, "Failed to read time from PCF85063ATL");
        return ESP_FAIL;
    }

    // Check oscillator stop flag
    if (data[0] & PCF85063_OSF_BIT) {
        ESP_LOGW(TAG, "PCF85063ATL oscillator was stopped - time may be invalid");
        return ESP_ERR_INVALID_STATE;
    }

    // Convert BCD to decimal
    struct tm timeinfo = {0};
    timeinfo.tm_sec = bcd_to_dec(data[0] & 0x7F);
    timeinfo.tm_min = bcd_to_dec(data[1] & 0x7F);
    timeinfo.tm_hour = bcd_to_dec(data[2] & 0x3F);
    timeinfo.tm_mday = bcd_to_dec(data[3] & 0x3F);
    timeinfo.tm_wday = bcd_to_dec(data[4] & 0x07);
    timeinfo.tm_mon = bcd_to_dec(data[5] & 0x1F) - 1;
    timeinfo.tm_year = bcd_to_dec(data[6]) + 100;

    *time_out = mktime(&timeinfo);
    return ESP_OK;
}

esp_err_t pcf85063_write_time(time_t time_in)
{
    if (!rtc_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!rtc_available)
        return ESP_ERR_NOT_FOUND;

    struct tm timeinfo;
    localtime_r(&time_in, &timeinfo);

    uint8_t data[7];
    data[0] = dec_to_bcd(timeinfo.tm_sec) & 0x7F;
    data[1] = dec_to_bcd(timeinfo.tm_min) & 0x7F;
    data[2] = dec_to_bcd(timeinfo.tm_hour) & 0x3F;
    data[3] = dec_to_bcd(timeinfo.tm_mday) & 0x3F;
    data[4] = dec_to_bcd(timeinfo.tm_wday) & 0x07;
    data[5] = dec_to_bcd(timeinfo.tm_mon + 1) & 0x1F;
    int year = timeinfo.tm_year + 1900;
    if (year < 2000) year = 2000;
    if (year > 2099) year = 2099;
    data[6] = dec_to_bcd(year - 2000);

    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_SECONDS, data, 7) != 0) {
        ESP_LOGE(TAG, "Failed to write time to PCF85063ATL");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote time to PCF85063ATL: %04d-%02d-%02d %02d:%02d:%02d",
             year, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return ESP_OK;
}

bool pcf85063_is_available(void)
{
    return rtc_available;
}

esp_err_t pcf85063_set_alarm(int hour, int minute)
{
    if (!rtc_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!rtc_available)
        return ESP_ERR_NOT_FOUND;

    /* 1. Clear AIE and AF in Control_2 before writing alarm regs */
    uint8_t ctrl2;
    if (board_bb_i2c_read(PCF85063_ADDR, PCF85063_ADDR_CONTROL_2, &ctrl2, 1) != 0)
        return ESP_FAIL;
    ctrl2 &= ~(PCF85063_AIE_BIT | PCF85063_AF_BIT);
    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_CONTROL_2, &ctrl2, 1) != 0)
        return ESP_FAIL;

    /* 2. Write alarm registers — enable hour and minute, disable the rest */
    uint8_t alarm[5] = {
        PCF85063_AEN_BIT,                /* seconds: disabled */
        dec_to_bcd(minute),              /* minutes: enabled (AEN=0) */
        dec_to_bcd(hour),                /* hours:   enabled (AEN=0) */
        PCF85063_AEN_BIT,                /* day:     disabled */
        PCF85063_AEN_BIT,                /* weekday: disabled */
    };
    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_SECOND_ALARM, alarm, 5) != 0)
        return ESP_FAIL;

    /* 3. Enable alarm interrupt (AIE) */
    ctrl2 |= PCF85063_AIE_BIT;
    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_CONTROL_2, &ctrl2, 1) != 0)
        return ESP_FAIL;

    ESP_LOGI(TAG, "Alarm set for %02d:%02d (INT on GPIO6)", hour, minute);
    return ESP_OK;
}

esp_err_t pcf85063_clear_alarm_flag(void)
{
    if (!rtc_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!rtc_available)
        return ESP_ERR_NOT_FOUND;

    uint8_t ctrl2;
    if (board_bb_i2c_read(PCF85063_ADDR, PCF85063_ADDR_CONTROL_2, &ctrl2, 1) != 0)
        return ESP_FAIL;
    ctrl2 &= ~PCF85063_AF_BIT;
    if (board_bb_i2c_write(PCF85063_ADDR, PCF85063_ADDR_CONTROL_2, &ctrl2, 1) != 0)
        return ESP_FAIL;

    return ESP_OK;
}

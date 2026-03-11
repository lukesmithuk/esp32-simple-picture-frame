#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit-banged I2C — bypasses IDF driver to avoid ESP32-S3 SCL glitch bug */
int board_bb_i2c_read(uint8_t dev_addr, uint8_t reg_addr,
                      uint8_t *data, uint8_t len);
int board_bb_i2c_write(uint8_t dev_addr, uint8_t reg_addr,
                       const uint8_t *data, uint8_t len);

/**
 * @brief Initialise all on-board hardware.
 *
 * Execution order:
 *   1. I2C bus recovery (9× SCL toggle + STOP)
 *   2. I2C master bus (SDA=47, SCL=48)
 *   3. AXP2101 PMIC — verify chip ID, full cmd_init
 *   4. PCF85063 RTC  — verify presence, clear STOP bit
 *
 * Must be called before any other board_* function.
 */
esp_err_t board_init(void);

/**
 * @brief Enable or disable EPD power rail (PMIC ALDO3 = EPD_VCC).
 *
 * Call board_epd_power(true) before epd_init().
 * Call board_epd_power(false) after epd_deinit().
 */
esp_err_t board_epd_power(bool on);

/**
 * @brief Enter PMIC sleep — cuts most LDOs.
 *
 * WARNING: disables DLDO1/DLDO2 which power the USB-JTAG serial bridge.
 * Do NOT call in debug builds where you need serial output.
 */
esp_err_t board_sleep(void);

/* ── RTC ──────────────────────────────────────────────────────────────── */

/** @brief Returns true if the PCF85063 was found and initialised. */
bool board_rtc_is_available(void);

/**
 * @brief Read current time from the RTC.
 * @return ESP_ERR_INVALID_STATE if the oscillator-stop flag is set (clock
 *         was never set or lost power).
 */
esp_err_t board_rtc_get_time(time_t *t);

/** @brief Write current time to the RTC. */
esp_err_t board_rtc_set_time(time_t t);

/**
 * @brief Set RTC alarm for a specific hour:minute:second.
 *
 * Triggers INT on GPIO6 (active LOW) when the time matches.
 * Day/weekday fields are disabled — alarm fires daily.
 */
esp_err_t board_rtc_set_alarm(int hour, int minute, int second);

/** @brief Clear the RTC alarm flag (must be called after each wakeup). */
esp_err_t board_rtc_clear_alarm_flag(void);

/**
 * @brief Configure deep sleep with RTC alarm wakeup and enter sleep.
 *
 * Sets EXT0 wakeup on GPIO6 (RTC INT, active LOW) then calls
 * esp_deep_sleep_start(). Does not return — chip resets on wake.
 */
void board_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif

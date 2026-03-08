/*
 * pmic.h — TG28 / AXP2101-compatible PMIC driver
 *
 * The TG28 is the PMIC fitted on the Waveshare ESP32-S3-PhotoPainter (newer
 * revision).  Its register layout is similar to older AXP chips (AXP192-era),
 * not the AXP2101 (which moved DC/LDO enable to 0x80/0x90).  The registers
 * confirmed working on hardware are documented in PROGRESS.md.
 *
 * Hardware-confirmed register map (from Phase 1 bring-up, 2026-02-21):
 *   0x03  CHIP_ID      read-only; TG28 = 0x4A
 *   0x11  LDO_EN_1     LDO enable group 1 (ALDO1 = bit0, ALDO2 = bit1, …)
 *   0x12  LDO_EN_2     LDO enable group 2:
 *                        bit2 = ALDO3 (EPD_VCC)
 *                        bit3 = ALDO4
 *                        bit4 = BLDO1
 *                        bit5 = BLDO2
 *   0x13  LDO_EN_3     LDO enable group 3:
 *                        bit0 = DLDO1
 *                        bit1 = DLDO2
 *                        bit6 = CPUSLDO
 *   0x1C  ALDO3_VOLT   ALDO3 voltage: [4:0] = (mV - 500) / 100
 *                        (range 500–3500 mV, 100 mV/step; code 28 = 3.3 V)
 *
 * Note: register 0x10 (DCDC_EN) bit→rail mapping is not yet confirmed for
 * TG28.  It is left untouched by pmic_sleep() until the mapping is
 * established.  See TODO.md "Phase 7 — Determine DCDC_EN bit mapping".
 *
 * Boot-cycle model
 * ----------------
 * This device uses deep sleep between display updates, not a traditional
 * main-loop.  After pmic_sleep() + esp_deep_sleep_start(), the ESP32 powers
 * down its CPU cores.  When the PCF85063 RTC alarm fires (GPIO6, active LOW),
 * the chip cold-boots and app_main() runs again from the top.
 *
 * The "main loop" is therefore:
 *   boot → init → update display → pmic_sleep() → deep_sleep_start()
 *     ↑                                                      |
 *     └───────────── RTC alarm → cold boot ──────────────────┘
 *
 * pmic_sleep() should be called immediately before esp_deep_sleep_start().
 * pmic_deinit() is optional before sleep — the PMIC hardware is not affected
 * by releasing the software handle.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque driver handle.  Allocated by pmic_init(), freed by pmic_deinit(). */
typedef struct pmic_dev_t *pmic_handle_t;

/*
 * pmic_init — initialise the PMIC driver.
 *
 * Adds a persistent I2C device handle for the TG28 (address 0x34) on the
 * supplied bus, then reads the chip ID register (0x03) and returns
 * ESP_ERR_NOT_SUPPORTED if the ID is not 0x4A.
 *
 * The caller owns the bus and must keep it open for the lifetime of the
 * handle.  Multiple devices may share the same bus handle.
 *
 * @param bus   Initialised i2c_master_bus_handle_t (shared with RTC, etc.)
 * @param out   Receives the PMIC handle on success; untouched on error.
 * @return      ESP_OK on success, ESP_ERR_NOT_SUPPORTED if chip ID mismatch,
 *              or an I2C error code.
 */
esp_err_t pmic_init(i2c_master_bus_handle_t bus, pmic_handle_t *out);

/*
 * pmic_epd_power — control the EPD power rail (ALDO3 → EPD_VCC, 3.3 V).
 *
 * enable = true:  sets ALDO3 voltage to 3.3 V, then asserts the enable bit.
 * enable = false: clears the enable bit (voltage register is left unchanged).
 *
 * Allow ~2 ms after enabling before asserting EPD SPI — the rail ramps up
 * in under 1 ms in practice but a small margin prevents glitches.
 *
 * @param h       Handle from pmic_init().
 * @param enable  true = power on, false = power off.
 * @return        ESP_OK, or an I2C error code.
 */
esp_err_t pmic_epd_power(pmic_handle_t h, bool enable);

/*
 * pmic_sleep — disable all LDO rails in preparation for deep sleep.
 *
 * Writes 0x00 to LDO_EN_1 (0x11), LDO_EN_2 (0x12), and LDO_EN_3 (0x13),
 * disabling ALDO1–ALDO4, BLDO1–BLDO2, DLDO1–DLDO2, and CPUSLDO.
 *
 * Register 0x10 (DCDC_EN) is intentionally left untouched: the DC1 rail
 * (3.3 V system power) must remain active to sustain the ESP32-S3 RTC domain
 * during deep sleep, and the TG28's exact bit→rail mapping for 0x10 is not
 * yet confirmed.  See TODO "Phase 7 — Determine DCDC_EN bit mapping".
 *
 * Call this immediately before esp_deep_sleep_start().  After deep sleep, the
 * PCF85063 RTC alarm wakes the device via GPIO6 and the chip cold-boots;
 * pmic_init() is called again at the top of app_main().
 *
 * @param h   Handle from pmic_init().
 * @return    ESP_OK, or an I2C error code.
 */
esp_err_t pmic_sleep(pmic_handle_t h);

/*
 * pmic_deinit — release the driver handle.
 *
 * Removes the I2C device handle from the bus.  Does not modify any PMIC
 * register state.  Safe to call before deep sleep, but not required —
 * deep sleep resets all software state anyway.
 *
 * @param h   Handle from pmic_init().  Becomes invalid after this call.
 */
void pmic_deinit(pmic_handle_t h);

/*
 * pmic_log_state — dump all key PMIC registers to the log.
 *
 * Reads and logs the current value of every register that affects power
 * rail state.  Useful at suite boundaries in test mode, and at boot to
 * confirm the PMIC is in the expected state.  Read-only; no side effects.
 *
 * @param h   Handle from pmic_init().
 */
void pmic_log_state(pmic_handle_t h);

/*
 * pmic_run_tests — run PMIC diagnostics and log results.
 *
 * Performs:
 *   1. Chip ID read (expects 0x4A)
 *   2. Register probe (dump key registers to log)
 *   3. Write/readback/restore test on IRQ_EN_1 (0x40)
 *   4. ALDO3 power cycle: enable → check, disable → check
 *
 * Returns ESP_OK if all steps pass, ESP_FAIL if any check fails.
 * Safe to call on a fully initialised handle; does not affect persistent
 * PMIC state (registers are restored after each test).
 *
 * @param h   Handle from pmic_init().
 * @return    ESP_OK if all tests pass, ESP_FAIL otherwise.
 */
esp_err_t pmic_run_tests(pmic_handle_t h);

#ifdef __cplusplus
}
#endif

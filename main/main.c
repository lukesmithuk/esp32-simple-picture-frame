/*
 * I2C Bring-Up / TG28 PMIC Verification
 *
 * Phase 1 hardware verification:
 *   - Scan I2C bus for all devices
 *   - Verify expected devices are present (TG28 @ 0x34, PCF85063 @ 0x51, SHTC3 @ 0x70)
 *   - Read chip ID register 0x03 from 0x34
 *   - Dump TG28 registers 0x00-0x4F and 0x80-0xBF for comparison against AXP2101 datasheet
 *   - Round-trip write test on IRQ enable register to confirm register writes work
 *
 * AXP2101 register map reference (from XPowersLib / datasheet):
 *   0x00  PMU_STATUS_1    — VBUS/battery/charging status flags
 *   0x01  PMU_STATUS_2    — charger state machine
 *   0x02  ICC_STATUS      — battery fuel gauge SOC
 *   0x03  CHIP_ID         — 0x47 for AXP2101
 *   0x04  PWRKEY_CFG      — power-key timing settings
 *   0x05  PWROFF_EN       — battery detect, power-off enable
 *   0x06  PWROFF_THR      — under-voltage / low-battery thresholds
 *   0x08  SLEEP_CFG       — sleep mode configuration
 *   0x09  HIBERNATE_CFG   — hibernate/wakeup config
 *   0x0A  WAKEUP_CFG      — wakeup pin config
 *   0x10  DCDC_EN         — DC-DC 1-5 enable bits
 *   0x11  LDO_EN_1        — LDO enable (chip-variant dependent)
 *   0x12  LDO_EN_2        — ALDO1-4, BLDO1-2 enable
 *   0x13  LDO_EN_3        — DLDO1-2, CPUSLDO enable
 *   0x15  DCDC1_VOLT      — DC-DC1 output voltage
 *   0x16  DCDC2_VOLT      — DC-DC2 output voltage
 *   0x17  DCDC3_VOLT      — DC-DC3 output voltage
 *   0x18  DCDC4_VOLT      — DC-DC4 output voltage
 *   0x19  DCDC5_VOLT      — DC-DC5 output voltage
 *   0x1A  ALDO1_VOLT      — ALDO1 voltage
 *   0x1B  ALDO2_VOLT      — ALDO2 voltage
 *   0x1C  ALDO3_VOLT      — ALDO3 voltage
 *   0x1D  ALDO4_VOLT      — ALDO4 voltage
 *   0x1E  BLDO1_VOLT      — BLDO1 voltage
 *   0x1F  BLDO2_VOLT      — BLDO2 voltage
 *   0x20  DLDO1_VOLT      — DLDO1 voltage
 *   0x21  DLDO2_VOLT      — DLDO2 voltage
 *   0x22  CPUSLDO_VOLT    — CPUSLDO voltage
 *   0x40  IRQ_EN_1        — IRQ enable register 1  ← write test target
 *   0x41  IRQ_EN_2        — IRQ enable register 2
 *   0x48  IRQ_STATUS_1    — IRQ latched status (write-1-to-clear)
 *   0x49  IRQ_STATUS_2    — IRQ latched status
 *   0x80+ ADC registers   — voltage/current/temp ADC results
 *
 * Record findings in PROGRESS.md before proceeding with firmware development.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bring_up";

/* Hardware pin assignments */
#define I2C_SDA_GPIO    47
#define I2C_SCL_GPIO    48
#define I2C_FREQ_HZ     100000   /* 100 kHz — conservative for bring-up */
#define I2C_PORT        I2C_NUM_0

/* Expected device addresses */
#define ADDR_PMIC       0x34     /* TG28 / AXP2101 */
#define ADDR_RTC        0x51     /* PCF85063 */
#define ADDR_SHTC3      0x70     /* SHTC3 temp/humidity */

/* AXP2101 registers */
#define AXP_REG_CHIP_ID      0x03
#define AXP_REG_DCDC_EN      0x10
#define AXP_REG_IRQ_EN1      0x40   /* IRQ enable 1 — write test target */

#define AXP2101_CHIP_ID      0x47

/* Timeout for individual I2C operations */
#define I2C_TIMEOUT_MS  50

/* -------------------------------------------------------------------------- */
/* I2C helpers                                                                 */
/*                                                                             */
/* Pattern from reference projects (aitjcize, multiverse2011):                */
/*   - Persistent device handle created once, reused for all transactions.    */
/*   - Call i2c_master_bus_wait_all_done() before each transaction.           */
/*     The probe scan leaves async state on the bus; without this wait,       */
/*     subsequent transmit_receive calls block indefinitely.                  */
/* -------------------------------------------------------------------------- */

static esp_err_t pmic_read(i2c_master_bus_handle_t bus,
                            i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_bus_wait_all_done(bus, I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t pmic_write(i2c_master_bus_handle_t bus,
                             i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    esp_err_t err = i2c_master_bus_wait_all_done(bus, I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, I2C_TIMEOUT_MS);
}


/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hardware Bring-Up: I2C Scan + TG28 Check ===");
    ESP_LOGI(TAG, "SDA=GPIO%d  SCL=GPIO%d  freq=%d Hz",
             I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);

    /* --- Init I2C master bus --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_PORT,
        .sda_io_num           = I2C_SDA_GPIO,
        .scl_io_num           = I2C_SCL_GPIO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_LOGI(TAG, "I2C master bus initialised");

    /* Persistent PMIC device handle — opened after probe scan, NULL until then */
    i2c_master_dev_handle_t pmic = NULL;

    /* --- Full address scan 0x01–0x77 --- */
    ESP_LOGI(TAG, "--- I2C bus scan ---");
    int found_count = 0;
    bool found[0x78] = {false};

    for (uint8_t addr = 0x01; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            found[addr] = true;
            found_count++;
        }
    }
    ESP_LOGI(TAG, "Scan complete: %d device(s) found", found_count);

    /* --- Check expected devices --- */
    ESP_LOGI(TAG, "--- Expected device check ---");
    struct { uint8_t addr; const char *name; } expected[] = {
        { ADDR_PMIC,  "TG28/AXP2101 PMIC"  },
        { ADDR_RTC,   "PCF85063 RTC"        },
        { ADDR_SHTC3, "SHTC3 Temp/Humidity" },
    };
    for (int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++) {
        if (found[expected[i].addr]) {
            ESP_LOGI(TAG, "  [PASS] 0x%02X %s", expected[i].addr, expected[i].name);
        } else {
            ESP_LOGE(TAG, "  [FAIL] 0x%02X %s — NOT FOUND", expected[i].addr, expected[i].name);
        }
    }

    if (!found[ADDR_PMIC]) {
        ESP_LOGE(TAG, "PMIC not found — skipping register probe");
        goto done;
    }

    /* Open a persistent device handle for all subsequent PMIC transactions.
     * Both reference projects (aitjcize, multiverse2011) use this pattern:
     * one handle created at init, reused for every read/write. Combined with
     * i2c_master_bus_wait_all_done() in each helper, this clears the async
     * state left on the bus by i2c_master_probe() and prevents hangs. */
    {
        i2c_device_config_t pmic_dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = ADDR_PMIC,
            .scl_speed_hz    = I2C_FREQ_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &pmic_dev_cfg, &pmic));
    }

    /* Chip ID */
    ESP_LOGI(TAG, "--- TG28 chip ID ---");
    {
        uint8_t chip_id = 0;
        esp_err_t err = pmic_read(bus, pmic, AXP_REG_CHIP_ID, &chip_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  Read failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "  Reg 0x03 (CHIP_ID) = 0x%02X  (AXP2101 = 0x47)", chip_id);
            if (chip_id == AXP2101_CHIP_ID) {
                ESP_LOGI(TAG, "  [PASS] Chip ID matches AXP2101");
            } else {
                ESP_LOGW(TAG, "  [INFO] Different chip ID — may still be register-compatible");
            }
        }
    }

    /* Read registers individually with before/after logging and a yield between
     * each read. This pinpoints exactly which register causes a hang. */
    ESP_LOGI(TAG, "--- Register probe (individual reads with yield) ---");
    {
        static const struct { uint8_t reg; const char *name; } probes[] = {
            { 0x00, "PMU_STATUS_1" },
            { 0x01, "PMU_STATUS_2" },
            { 0x03, "CHIP_ID" },
            { 0x08, "SLEEP_CFG" },
            { 0x10, "DCDC_EN" },
            { 0x11, "LDO_EN_1" },
            { 0x12, "LDO_EN_2" },
            { 0x13, "LDO_EN_3" },
            { 0x15, "DCDC1_VOLT" },
            { 0x16, "DCDC2_VOLT" },
            { 0x1A, "ALDO1_VOLT" },
            { 0x1B, "ALDO2_VOLT" },
            { 0x40, "IRQ_EN_1" },
            { 0x41, "IRQ_EN_2" },
        };
        for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
            vTaskDelay(pdMS_TO_TICKS(10)); /* yield + allow USB drain */
            ESP_LOGI(TAG, "    reading 0x%02X %s ...", probes[i].reg, probes[i].name);
            uint8_t val = 0;
            esp_err_t err = pmic_read(bus, pmic, probes[i].reg, &val);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "    0x%02X %-14s = 0x%02X", probes[i].reg, probes[i].name, val);
            } else {
                ESP_LOGE(TAG, "    0x%02X %-14s = ERROR (%s)",
                         probes[i].reg, probes[i].name, esp_err_to_name(err));
            }
        }
    }

    /* Write test — IRQ enable register 0x40
     *
     * Goal: confirm register writes are accepted (not silently discarded).
     * IRQ_EN_1 (0x40) is safe to briefly modify: masking IRQs has no effect
     * on power delivery or system stability.
     *
     * Procedure:
     *   1. Read original value
     *   2. Write bitwise-inverted value
     *   3. Read back — must differ from original
     *   4. Restore original value
     *   5. Read back — must match original again
     */
    ESP_LOGI(TAG, "--- Register write test (0x%02X IRQ_EN_1) ---", AXP_REG_IRQ_EN1);
    {
        uint8_t original = 0, readback = 0, restored = 0;

        esp_err_t err = pmic_read(bus, pmic, AXP_REG_IRQ_EN1, &original);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  Read failed: %s", esp_err_to_name(err));
            goto write_test_done;
        }
        ESP_LOGI(TAG, "  Original  = 0x%02X", original);

        uint8_t inverted = ~original;
        err = pmic_write(bus, pmic, AXP_REG_IRQ_EN1, inverted);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  Write failed: %s", esp_err_to_name(err));
            pmic_write(bus, pmic, AXP_REG_IRQ_EN1, original);
            goto write_test_done;
        }

        pmic_read(bus, pmic, AXP_REG_IRQ_EN1, &readback);
        ESP_LOGI(TAG, "  Wrote     = 0x%02X  readback = 0x%02X", inverted, readback);

        pmic_write(bus, pmic, AXP_REG_IRQ_EN1, original);
        pmic_read(bus, pmic, AXP_REG_IRQ_EN1, &restored);
        ESP_LOGI(TAG, "  Restored  = 0x%02X  readback = 0x%02X", original, restored);

        if (readback == inverted && restored == original) {
            ESP_LOGI(TAG, "  [PASS] Register writes accepted — TG28 register map is writable");
            ESP_LOGI(TAG, "  => Strong evidence TG28 is AXP2101 register-compatible");
        } else if (readback == original) {
            ESP_LOGW(TAG, "  [INFO] Readback unchanged — register may be read-only or masked");
        } else {
            ESP_LOGW(TAG, "  [INFO] Unexpected readback 0x%02X", readback);
        }
    }

write_test_done:
done:
    ESP_LOGI(TAG, "=== Bring-up complete ===");
    ESP_LOGI(TAG, "Record register dump in PROGRESS.md and compare to AXP2101 datasheet defaults.");

    if (pmic) i2c_master_bus_rm_device(pmic);
    i2c_del_master_bus(bus);
}

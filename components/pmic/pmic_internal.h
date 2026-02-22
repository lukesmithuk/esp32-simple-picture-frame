/*
 * pmic_internal.h — private implementation details for the pmic component.
 *
 * Included only by pmic.c and pmic_tests.c.  Do not include from outside
 * the component — use pmic.h for the public API.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* ── I2C device constants ────────────────────────────────────────────────── */

#define PMIC_I2C_ADDR       0x34
#define PMIC_I2C_FREQ_HZ    100000
#define PMIC_I2C_TIMEOUT_MS 50

/* ── Register addresses (hardware-confirmed on TG28) ─────────────────────── */

#define REG_CHIP_ID     0x03
#define REG_LDO_EN_1    0x11   /* ALDO1 = bit0, ALDO2 = bit1, … */
#define REG_LDO_EN_2    0x12   /* ALDO3 = bit2, ALDO4 = bit3, BLDO1 = bit4, BLDO2 = bit5 */
#define REG_LDO_EN_3    0x13   /* DLDO1 = bit0, DLDO2 = bit1, CPUSLDO = bit6 */
#define REG_ALDO3_VOLT  0x1C   /* [4:0] = (mV - 500) / 100 */
#define REG_IRQ_EN_1    0x40

/* ── Register values ─────────────────────────────────────────────────────── */

#define CHIP_ID_TG28        0x4A   /* TG28 and AXP2101 both report 0x4A */
#define BIT_ALDO3_EN        (1u << 2)   /* LDO_EN_2 bit 2 */
#define ALDO3_VOLT_CODE_3V3 28u         /* (3300 - 500) / 100 = 28 → 3.3 V */

/* ── Driver state ────────────────────────────────────────────────────────── */

struct pmic_dev_t {
    i2c_master_dev_handle_t i2c_dev;
};

/* ── Internal I2C helpers ────────────────────────────────────────────────── */

static inline esp_err_t reg_read(const struct pmic_dev_t *dev,
                                  uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, val, 1,
                                       PMIC_I2C_TIMEOUT_MS);
}

static inline esp_err_t reg_write(const struct pmic_dev_t *dev,
                                   uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev->i2c_dev, buf, 2, PMIC_I2C_TIMEOUT_MS);
}

/* Read register, apply mask, write back.  Leaves unmasked bits unchanged. */
static inline esp_err_t reg_set_bits(const struct pmic_dev_t *dev,
                                      uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t val;
    esp_err_t err = reg_read(dev, reg, &val);
    if (err != ESP_OK) return err;
    val = (val & ~mask) | (bits & mask);
    return reg_write(dev, reg, val);
}

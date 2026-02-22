/*
 * pmic.c — TG28 / AXP2101-compatible PMIC driver
 *
 * See pmic.h for the register map, hardware notes, and boot-cycle model.
 * See pmic_internal.h for the struct, register macros, and I2C helpers.
 * See pmic_tests.c for diagnostics (pmic_run_tests).
 */

#include "pmic.h"
#include "pmic_internal.h"

#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "pmic";

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t pmic_init(i2c_master_bus_handle_t bus, pmic_handle_t *out)
{
    struct pmic_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PMIC_I2C_ADDR,
        .scl_speed_hz    = PMIC_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    uint8_t chip_id = 0;
    err = reg_read(dev, REG_CHIP_ID, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip ID read failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
        return err;
    }

    if (chip_id != CHIP_ID_TG28) {
        ESP_LOGE(TAG, "unexpected chip ID 0x%02X (expected 0x%02X)",
                 chip_id, CHIP_ID_TG28);
        i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "init OK, chip ID 0x%02X", chip_id);
    *out = dev;
    return ESP_OK;
}

esp_err_t pmic_epd_power(pmic_handle_t h, bool enable)
{
    esp_err_t err;

    if (enable) {
        /* Set voltage before enabling to avoid a momentary undervoltage glitch */
        err = reg_write(h, REG_ALDO3_VOLT, ALDO3_VOLT_CODE_3V3);
        if (err != ESP_OK) return err;

        err = reg_set_bits(h, REG_LDO_EN_2, BIT_ALDO3_EN, BIT_ALDO3_EN);
        if (err != ESP_OK) return err;

        ESP_LOGI(TAG, "EPD power ON (ALDO3 = 3.3 V)");
    } else {
        err = reg_set_bits(h, REG_LDO_EN_2, BIT_ALDO3_EN, 0);
        if (err != ESP_OK) return err;

        ESP_LOGI(TAG, "EPD power OFF");
    }

    return ESP_OK;
}

esp_err_t pmic_sleep(pmic_handle_t h)
{
    /*
     * Disable all LDO rails before entering deep sleep.
     *
     * Writing 0x00 to each LDO enable register turns off:
     *   LDO_EN_1 (0x11): ALDO1, ALDO2 (and any others in this group)
     *   LDO_EN_2 (0x12): ALDO3 (EPD_VCC), ALDO4, BLDO1, BLDO2
     *   LDO_EN_3 (0x13): DLDO1, DLDO2, CPUSLDO
     *
     * Register 0x10 (DCDC_EN) is intentionally NOT modified.  DC1 (the 3.3 V
     * system rail) must remain active to sustain the ESP32-S3 RTC domain
     * during deep sleep.  The TG28's exact bit→rail mapping for register 0x10
     * has not yet been confirmed — touching it risks disabling DC1 and causing
     * a reset.  See TODO "Phase 7 — Determine DCDC_EN bit mapping".
     *
     * After this call, the caller should invoke esp_deep_sleep_start().  The
     * PCF85063 RTC alarm will wake the device via GPIO6, triggering a cold
     * boot.  app_main() and pmic_init() run again from the top — there is no
     * resume from this point.
     */
    esp_err_t err;

    err = reg_write(h, REG_LDO_EN_1, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sleep: LDO_EN_1 write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = reg_write(h, REG_LDO_EN_2, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sleep: LDO_EN_2 write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = reg_write(h, REG_LDO_EN_3, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sleep: LDO_EN_3 write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LDO rails off; DC rails untouched — entering deep sleep");
    return ESP_OK;
}

void pmic_deinit(pmic_handle_t h)
{
    if (!h) return;
    i2c_master_bus_rm_device(h->i2c_dev);
    free(h);
}

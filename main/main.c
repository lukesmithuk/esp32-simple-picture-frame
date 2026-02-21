/*
 * I2C Bring-Up / TG28 PMIC Verification
 *
 * Phase 1 hardware verification:
 *   - Scan I2C bus for all devices
 *   - Verify expected devices are present (TG28 @ 0x34, PCF85063 @ 0x51, SHTC3 @ 0x70)
 *   - Read chip ID register 0x03 from 0x34:
 *       0x47 → AXP2101-compatible (good path)
 *       other → document actual value, plan custom TG28 driver
 *
 * Record findings in PROGRESS.md before proceeding with firmware development.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "bring_up";

/* Hardware pin assignments (from schematic / CLAUDE.md) */
#define I2C_SDA_GPIO    47
#define I2C_SCL_GPIO    48
#define I2C_FREQ_HZ     100000   /* 100 kHz — conservative for bring-up */
#define I2C_PORT        I2C_NUM_0

/* Expected device addresses */
#define ADDR_PMIC       0x34     /* TG28 / AXP2101 */
#define ADDR_RTC        0x51     /* PCF85063 */
#define ADDR_SHTC3      0x70     /* SHTC3 temp/humidity */

/* AXP2101 / TG28 registers */
#define AXP_REG_CHIP_ID 0x03     /* chip ID register; AXP2101 returns 0x47 */
#define AXP2101_CHIP_ID 0x47

/* Timeout for individual I2C probes */
#define PROBE_TIMEOUT_MS  10

/* -------------------------------------------------------------------------- */
/* I2C helpers using new-style i2c_master API (IDF v5.x+)                    */
/* -------------------------------------------------------------------------- */

/*
 * read_register() — write 1-byte register address then read 1-byte value.
 * Returns ESP_OK and sets *val on success.
 */
static esp_err_t read_register(i2c_master_bus_handle_t bus, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *val)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = dev_addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };

    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf = reg_addr;
    err = i2c_master_transmit_receive(dev, &buf, 1, val, 1,
                                      PROBE_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return err;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hardware Bring-Up: I2C Scan + TG28 Check ===");
    ESP_LOGI(TAG, "SDA=GPIO%d  SCL=GPIO%d  freq=%d Hz", I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);

    /* --- Init I2C master bus --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port        = I2C_PORT,
        .sda_io_num      = I2C_SDA_GPIO,
        .scl_io_num      = I2C_SCL_GPIO,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_LOGI(TAG, "I2C master bus initialised");

    /* --- Full address scan 0x01–0x77 --- */
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int found_count = 0;
    bool found[0x78] = {false};

    for (uint8_t addr = 0x01; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            found[addr] = true;
            found_count++;
        }
    }

    if (found_count == 0) {
        ESP_LOGW(TAG, "No I2C devices found — check wiring and pull-ups");
    } else {
        ESP_LOGI(TAG, "Scan complete: %d device(s) found", found_count);
    }

    /* --- Check expected devices --- */
    ESP_LOGI(TAG, "--- Expected device check ---");

    struct {
        uint8_t     addr;
        const char *name;
    } expected[] = {
        { ADDR_PMIC,  "TG28/AXP2101 PMIC"   },
        { ADDR_RTC,   "PCF85063 RTC"         },
        { ADDR_SHTC3, "SHTC3 Temp/Humidity"  },
    };

    for (int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++) {
        if (found[expected[i].addr]) {
            ESP_LOGI(TAG, "  [PASS] 0x%02X %s", expected[i].addr, expected[i].name);
        } else {
            ESP_LOGE(TAG, "  [FAIL] 0x%02X %s — NOT FOUND", expected[i].addr, expected[i].name);
        }
    }

    /* --- TG28 chip ID check (register 0x03) --- */
    ESP_LOGI(TAG, "--- TG28/AXP2101 Chip ID ---");

    if (!found[ADDR_PMIC]) {
        ESP_LOGE(TAG, "  Skipping — no device at 0x%02X", ADDR_PMIC);
    } else {
        uint8_t chip_id = 0;
        esp_err_t err = read_register(bus, ADDR_PMIC, AXP_REG_CHIP_ID, &chip_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  Failed to read reg 0x%02X from 0x%02X: %s",
                     AXP_REG_CHIP_ID, ADDR_PMIC, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "  Reg 0x%02X = 0x%02X", AXP_REG_CHIP_ID, chip_id);
            if (chip_id == AXP2101_CHIP_ID) {
                ESP_LOGI(TAG, "  [PASS] TG28 is AXP2101-compatible (0x47)");
            } else {
                ESP_LOGW(TAG, "  [INFO] Chip ID 0x%02X != AXP2101 (0x47)", chip_id);
                ESP_LOGW(TAG, "         TG28 may NOT be register-compatible.");
                ESP_LOGW(TAG, "         Record this value and plan a custom TG28 driver.");
            }
        }
    }

    /* --- Summary --- */
    ESP_LOGI(TAG, "=== Bring-up complete ===");
    ESP_LOGI(TAG, "Record these findings in PROGRESS.md:");
    ESP_LOGI(TAG, "  - List of found I2C addresses");
    ESP_LOGI(TAG, "  - PMIC chip ID register value");
    ESP_LOGI(TAG, "  - Any unexpected devices on bus");
    ESP_LOGI(TAG, "Then proceed to Phase 2 (EPD power-on / white fill).");

    /* Clean up */
    i2c_del_master_bus(bus);
}

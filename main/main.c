/*
 * Phase 1 Hardware Bring-Up
 *
 * Tests run in sequence:
 *   1. I2C bus scan (0x01–0x77)
 *   2. TG28 PMIC register probe + write test
 *   3. EPD power: set ALDO3 to 3.3V, enable, check BUSY pin (GPIO13)
 *   4. SD card: mount 4-bit SDIO (GPIO38/39/40/41/1/2), list root dir
 *
 * Key hardware findings (from schematic 2026-02-21):
 *   - EPD_VCC = TG28 ALDO3 (LDO_EN_2 reg 0x12 bit 2).  No EPD_PWR GPIO.
 *   - EPD_BUSY = GPIO13, active LOW (busy/init), HIGH when idle/ready
 *   - SD card = 4-bit SDIO:  D3/CS=38, CLK=39, D0=40, CMD=41, D1=1, D2=2
 *   - RTC_INT = GPIO6 (direct to ESP32, not through TG28)
 *
 * AXP2101 / TG28 register reference (register-compatible, TG28 chip ID = 0x4A):
 *   0x03  CHIP_ID         0x47 AXP2101 / 0x4A TG28
 *   0x10  DCDC_EN         DC-DC 1-5 enable bits
 *   0x11  LDO_EN_1        LDO enable group 1
 *   0x12  LDO_EN_2        bit2=ALDO3, bit3=ALDO4, bit4=BLDO1, bit5=BLDO2
 *   0x13  LDO_EN_3        bit0=DLDO1, bit1=DLDO2, bit6=CPUSLDO
 *   0x1C  ALDO3_VOLT      [4:0] = (mV - 500) / 100  (range 500–3500 mV, 100 mV/step)
 *   0x40  IRQ_EN_1        IRQ enable register 1
 *   0x41  IRQ_EN_2        IRQ enable register 2
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bring_up";

/* ── Pin assignments ─────────────────────────────────────────────────────── */

#define I2C_SDA_GPIO    47
#define I2C_SCL_GPIO    48
#define I2C_FREQ_HZ     100000

#define EPD_BUSY_GPIO   13   /* active LOW: LOW=busy, HIGH=idle */

#define SD_CLK_GPIO     39
#define SD_CMD_GPIO     41
#define SD_D0_GPIO      40
#define SD_D1_GPIO       1
#define SD_D2_GPIO       2
#define SD_D3_GPIO      38

/* ── I2C device addresses ────────────────────────────────────────────────── */

#define ADDR_PMIC       0x34
#define ADDR_RTC        0x51
#define ADDR_SHTC3      0x70

/* ── TG28 / AXP2101 registers ────────────────────────────────────────────── */

#define AXP_REG_CHIP_ID      0x03
#define AXP_REG_LDO_EN_2     0x12   /* bit 2 = ALDO3 enable */
#define AXP_REG_ALDO3_VOLT   0x1C   /* [4:0] voltage code */
#define AXP_REG_IRQ_EN1      0x40

#define AXP_LDO_EN2_ALDO3    (1u << 2)

/* ALDO3 = 3.3 V: code = (3300 - 500) / 100 = 28 */
#define ALDO3_VOLT_3V3       28

#define I2C_TIMEOUT_MS       50

/* ── I2C helpers ─────────────────────────────────────────────────────────── */

static esp_err_t pmic_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t pmic_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, I2C_TIMEOUT_MS);
}

/* ── Test 1 + 2: I2C scan and PMIC register probe ─────────────────────────── */

static i2c_master_dev_handle_t test_i2c_and_pmic(i2c_master_bus_handle_t bus)
{
    /* Scan */
    ESP_LOGI(TAG, "--- I2C bus scan ---");
    int found_count = 0;
    bool found[0x78] = {false};
    for (uint8_t addr = 0x01; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  Found 0x%02X", addr);
            found[addr] = true;
            found_count++;
        }
    }
    ESP_LOGI(TAG, "Scan complete: %d device(s)", found_count);

    struct { uint8_t addr; const char *name; } expected[] = {
        { ADDR_PMIC,  "TG28/AXP2101 PMIC"  },
        { ADDR_RTC,   "PCF85063 RTC"        },
        { ADDR_SHTC3, "SHTC3 Temp/Humidity" },
    };
    for (int i = 0; i < (int)(sizeof(expected)/sizeof(expected[0])); i++) {
        if (found[expected[i].addr])
            ESP_LOGI(TAG, "  [PASS] 0x%02X %s", expected[i].addr, expected[i].name);
        else
            ESP_LOGE(TAG, "  [FAIL] 0x%02X %s — NOT FOUND", expected[i].addr, expected[i].name);
    }

    if (!found[ADDR_PMIC]) {
        ESP_LOGE(TAG, "PMIC not found — skipping PMIC tests");
        return NULL;
    }

    /* Open persistent PMIC device handle */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADDR_PMIC,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t pmic = NULL;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &pmic));

    /* Chip ID */
    ESP_LOGI(TAG, "--- TG28 chip ID ---");
    uint8_t chip_id = 0;
    if (pmic_read(pmic, AXP_REG_CHIP_ID, &chip_id) == ESP_OK)
        ESP_LOGI(TAG, "  Reg 0x03 = 0x%02X  (AXP2101=0x47, TG28=0x4A)", chip_id);
    else
        ESP_LOGE(TAG, "  Chip ID read failed");

    /* Register probe */
    ESP_LOGI(TAG, "--- Register probe ---");
    static const struct { uint8_t reg; const char *name; } probes[] = {
        { 0x00, "PMU_STATUS_1" }, { 0x01, "PMU_STATUS_2" }, { 0x03, "CHIP_ID"     },
        { 0x08, "SLEEP_CFG"   }, { 0x10, "DCDC_EN"      }, { 0x11, "LDO_EN_1"   },
        { 0x12, "LDO_EN_2"    }, { 0x13, "LDO_EN_3"     }, { 0x15, "DCDC1_VOLT" },
        { 0x16, "DCDC2_VOLT"  }, { 0x1B, "ALDO2_VOLT"   }, { 0x1C, "ALDO3_VOLT" },
        { 0x40, "IRQ_EN_1"    }, { 0x41, "IRQ_EN_2"     },
    };
    for (int i = 0; i < (int)(sizeof(probes)/sizeof(probes[0])); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t val = 0;
        esp_err_t err = pmic_read(pmic, probes[i].reg, &val);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "  0x%02X %-14s = 0x%02X", probes[i].reg, probes[i].name, val);
        else
            ESP_LOGE(TAG, "  0x%02X %-14s = ERROR (%s)", probes[i].reg, probes[i].name, esp_err_to_name(err));
    }

    /* Write test */
    ESP_LOGI(TAG, "--- Register write test (IRQ_EN_1 0x40) ---");
    uint8_t orig = 0, rb1 = 0, rb2 = 0;
    pmic_read(pmic, AXP_REG_IRQ_EN1, &orig);
    pmic_write(pmic, AXP_REG_IRQ_EN1, (uint8_t)~orig);
    pmic_read(pmic, AXP_REG_IRQ_EN1, &rb1);
    pmic_write(pmic, AXP_REG_IRQ_EN1, orig);
    pmic_read(pmic, AXP_REG_IRQ_EN1, &rb2);
    if (rb1 == (uint8_t)~orig && rb2 == orig)
        ESP_LOGI(TAG, "  [PASS] write/readback/restore correct");
    else
        ESP_LOGW(TAG, "  [WARN] orig=0x%02X wrote=0x%02X rb=0x%02X restored=0x%02X",
                 orig, (uint8_t)~orig, rb1, rb2);

    return pmic;
}

/* ── Test 3: EPD power via TG28 ALDO3 ───────────────────────────────────── */

static void test_epd_power(i2c_master_dev_handle_t pmic)
{
    ESP_LOGI(TAG, "--- EPD power test (TG28 ALDO3 -> EPD_VCC) ---");

    /* Configure EPD_BUSY (GPIO13) as input with pull-up.
     * Active LOW: LOW = busy/no-power, HIGH = idle/ready. */
    gpio_config_t io_cfg = {
        .pin_bit_mask    = (1ULL << EPD_BUSY_GPIO),
        .mode            = GPIO_MODE_INPUT,
        .pull_up_en      = GPIO_PULLUP_ENABLE,
        .pull_down_en    = GPIO_PULLDOWN_DISABLE,
        .intr_type       = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    int busy_before = gpio_get_level(EPD_BUSY_GPIO);
    ESP_LOGI(TAG, "  BUSY before power-on: %s", busy_before ? "HIGH" : "LOW");

    /* Set ALDO3 voltage to 3.3 V before enabling.
     * Encoding: [4:0] = (mV - 500) / 100, so 3300 mV -> code 28 (0x1C). */
    pmic_write(pmic, AXP_REG_ALDO3_VOLT, ALDO3_VOLT_3V3);
    uint8_t volt = 0;
    pmic_read(pmic, AXP_REG_ALDO3_VOLT, &volt);
    ESP_LOGI(TAG, "  ALDO3_VOLT (0x1C) = 0x%02X -> %.1f V",
             volt, 0.5f + (float)(volt & 0x1F) * 0.1f);

    /* Enable ALDO3: set bit 2 of LDO_EN_2 (reg 0x12) */
    uint8_t ldo_en2 = 0;
    pmic_read(pmic, AXP_REG_LDO_EN_2, &ldo_en2);
    ESP_LOGI(TAG, "  LDO_EN_2 before: 0x%02X", ldo_en2);
    pmic_write(pmic, AXP_REG_LDO_EN_2, ldo_en2 | AXP_LDO_EN2_ALDO3);
    uint8_t ldo_en2_after = 0;
    pmic_read(pmic, AXP_REG_LDO_EN_2, &ldo_en2_after);
    ESP_LOGI(TAG, "  LDO_EN_2 after:  0x%02X", ldo_en2_after);

    if (!(ldo_en2_after & AXP_LDO_EN2_ALDO3)) {
        ESP_LOGE(TAG, "  [FAIL] ALDO3 enable bit did not stick");
        return;
    }
    ESP_LOGI(TAG, "  ALDO3 enabled");

    /* Poll BUSY for up to 2 s — EPD powers up and releases BUSY -> HIGH */
    ESP_LOGI(TAG, "  Waiting for EPD_BUSY to settle (up to 2 s)...");
    int busy = 0;
    for (int ms = 0; ms <= 2000; ms += 50) {
        busy = gpio_get_level(EPD_BUSY_GPIO);
        if (busy) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "  BUSY after power-on: %s", busy ? "HIGH" : "LOW");

    if (busy_before == 0 && busy == 1)
        ESP_LOGI(TAG, "  [PASS] BUSY went LOW->HIGH — EPD_VCC confirmed working");
    else if (busy_before == 1 && busy == 1)
        ESP_LOGI(TAG, "  [INFO] BUSY was already HIGH (EPD may have been powered, or pull-up dominant)");
    else if (busy == 0)
        ESP_LOGW(TAG, "  [WARN] BUSY still LOW after 2 s — EPD may still be initialising or ALDO3 not reaching panel");
    else
        ESP_LOGI(TAG, "  [INFO] BUSY: before=%d after=%d", busy_before, busy);

    /* Leave ALDO3 enabled for any subsequent EPD operations this session */
}

/* ── Test 4: SD card mount ───────────────────────────────────────────────── */

static void test_sd_card(void)
{
    ESP_LOGI(TAG, "--- SD card test (4-bit SDIO) ---");
    ESP_LOGI(TAG, "  CLK=%-2d  CMD=%-2d  D0=%-2d  D1=%-2d  D2=%-2d  D3=%-2d",
             SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO, SD_D1_GPIO, SD_D2_GPIO, SD_D3_GPIO);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;  /* 400 kHz — safe for bring-up */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SD_CLK_GPIO;
    slot.cmd   = SD_CMD_GPIO;
    slot.d0    = SD_D0_GPIO;
    slot.d1    = SD_D1_GPIO;
    slot.d2    = SD_D2_GPIO;
    slot.d3    = SD_D3_GPIO;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  [FAIL] Mount failed: %s", esp_err_to_name(ret));
        if (ret == ESP_FAIL)
            ESP_LOGE(TAG, "         (Card present but filesystem unreadable — try FAT32 format)");
        else if (ret == ESP_ERR_NOT_FOUND)
            ESP_LOGE(TAG, "         (No card detected)");
        return;
    }

    ESP_LOGI(TAG, "  [PASS] Mounted at /sdcard");
    sdmmc_card_print_info(stdout, card);

    /* List root directory */
    ESP_LOGI(TAG, "  Root directory:");
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGE(TAG, "  opendir failed");
    } else {
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "    %-30s  %s",
                     entry->d_name,
                     entry->d_type == DT_DIR ? "DIR" : "file");
            count++;
        }
        closedir(dir);
        ESP_LOGI(TAG, "  %d entr%s", count, count == 1 ? "y" : "ies");
        ESP_LOGI(TAG, "  [PASS] SD card directory listing complete");
    }

    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    ESP_LOGI(TAG, "  Unmounted");
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Phase 1 Bring-Up");
    ESP_LOGI(TAG, " I2C SDA=%d SCL=%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
    ESP_LOGI(TAG, "========================================");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = I2C_SDA_GPIO,
        .scl_io_num           = I2C_SCL_GPIO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_master_dev_handle_t pmic = test_i2c_and_pmic(bus);

    if (pmic) {
        test_epd_power(pmic);
        i2c_master_bus_rm_device(pmic);
    }

    i2c_del_master_bus(bus);

    test_sd_card();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Bring-up complete");
    ESP_LOGI(TAG, "========================================");
}

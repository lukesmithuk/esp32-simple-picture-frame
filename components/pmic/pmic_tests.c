/*
 * pmic_tests.c — PMIC diagnostic tests
 *
 * Implements pmic_run_tests(), declared in pmic.h.
 * Uses pmic_internal.h for direct register access so tests can verify
 * hardware state independently of the public API.
 *
 * Test lifecycle
 * --------------
 * Every individual test is run via run_test(), which wraps it with:
 *
 *   setup()    — write the known pre-test PMIC state
 *   test fn()  — the actual test logic
 *   teardown() — write the known post-test PMIC state (identical to setup)
 *
 * Because setup and teardown establish the same explicit state, every test
 * starts from a clean slate and leaves the board in a clean slate.  There is
 * no save/restore: if the correct state is written unconditionally, it is
 * always correct regardless of what the test did.
 *
 * Known state before and after each test:
 *   - ALDO3 (EPD_VCC) off  — EPD has no power; avoids interference from
 *                            ALDO3 state left by a previous run
 *   - IRQ_EN_1 (0x40) = 0xFF — all IRQs enabled (power-on default)
 */

#include "pmic.h"
#include "pmic_internal.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pmic";

/* ── Setup / teardown ────────────────────────────────────────────────────── */

/* Establish the known pre/post-test PMIC state. */
static void setup(pmic_handle_t h)
{
    pmic_epd_power(h, false);           /* ALDO3 off */
    reg_write(h, REG_IRQ_EN_1, 0xFF);   /* IRQs at default */
}

static void teardown(pmic_handle_t h)
{
    pmic_epd_power(h, false);           /* ALDO3 off */
    reg_write(h, REG_IRQ_EN_1, 0xFF);   /* IRQs at default */
}

/*
 * run_test — execute one test function wrapped in setup/teardown.
 */
static bool run_test(pmic_handle_t h, bool (*fn)(pmic_handle_t))
{
    setup(h);
    bool result = fn(h);
    teardown(h);
    return result;
}

/* ── Individual test functions ───────────────────────────────────────────── */

static bool test_chip_id(pmic_handle_t h)
{
    ESP_LOGI(TAG, "--- PMIC test 1: chip ID ---");
    uint8_t chip_id = 0;
    esp_err_t err = reg_read(h, REG_CHIP_ID, &chip_id);
    if (err != ESP_OK || chip_id != CHIP_ID_TG28) {
        ESP_LOGE(TAG, "  [FAIL] chip ID 0x%02X (err %s)", chip_id, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "  [PASS] chip ID 0x%02X", chip_id);
    return true;
}

static bool test_register_probe(pmic_handle_t h)
{
    ESP_LOGI(TAG, "--- PMIC test 2: register probe ---");
    static const struct { uint8_t reg; const char *name; } probes[] = {
        { 0x00, "PMU_STATUS_1" }, { 0x01, "PMU_STATUS_2" }, { 0x03, "CHIP_ID"      },
        { 0x10, "DCDC_EN"      }, { 0x11, "LDO_EN_1"    }, { 0x12, "LDO_EN_2"    },
        { 0x13, "LDO_EN_3"     }, { 0x16, "VBUS_CUR_LIM" }, { 0x1C, "ALDO3_VOLT" },
        { 0x1D, "ALDO4_VOLT"   }, { 0x40, "IRQ_EN_1"    }, { 0x41, "IRQ_EN_2"    },
    };
    bool pass = true;
    for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t val = 0;
        esp_err_t err = reg_read(h, probes[i].reg, &val);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "  0x%02X %-14s = 0x%02X", probes[i].reg, probes[i].name, val);
        else {
            ESP_LOGE(TAG, "  0x%02X %-14s = ERROR (%s)", probes[i].reg, probes[i].name,
                     esp_err_to_name(err));
            pass = false;
        }
    }
    return pass;
}

static bool test_write_readback(pmic_handle_t h)
{
    ESP_LOGI(TAG, "--- PMIC test 3: write/readback (IRQ_EN_1 0x40) ---");

    /* setup() guarantees IRQ_EN_1 = 0xFF entering here */
    uint8_t orig = 0xFF;

    reg_write(h, REG_IRQ_EN_1, 0x00);
    uint8_t rb_zero = 0;
    reg_read(h, REG_IRQ_EN_1, &rb_zero);

    reg_write(h, REG_IRQ_EN_1, orig);
    uint8_t rb_orig = 0;
    reg_read(h, REG_IRQ_EN_1, &rb_orig);

    if (rb_zero != 0x00 || rb_orig != orig) {
        ESP_LOGE(TAG, "  [FAIL] wrote 0x00 rb=0x%02X, wrote 0xFF rb=0x%02X",
                 rb_zero, rb_orig);
        return false;
    }
    ESP_LOGI(TAG, "  [PASS] write/readback correct (0x00 and 0xFF both verified)");
    return true;
}

static bool test_aldo3_power_cycle(pmic_handle_t h)
{
    ESP_LOGI(TAG, "--- PMIC test 4: ALDO3 enable / disable ---");

    /* setup() guarantees ALDO3 is off entering here */
    bool pass = true;

    pmic_epd_power(h, true);
    uint8_t ldo2_on = 0;
    reg_read(h, REG_LDO_EN_2, &ldo2_on);
    if (!(ldo2_on & BIT_ALDO3_EN)) {
        ESP_LOGE(TAG, "  [FAIL] ALDO3 enable — LDO_EN_2=0x%02X", ldo2_on);
        pass = false;
    } else {
        ESP_LOGI(TAG, "  [PASS] ALDO3 enabled (LDO_EN_2=0x%02X)", ldo2_on);
    }

    pmic_epd_power(h, false);
    uint8_t ldo2_off = 0;
    reg_read(h, REG_LDO_EN_2, &ldo2_off);
    if (ldo2_off & BIT_ALDO3_EN) {
        ESP_LOGE(TAG, "  [FAIL] ALDO3 disable — LDO_EN_2=0x%02X", ldo2_off);
        pass = false;
    } else {
        ESP_LOGI(TAG, "  [PASS] ALDO3 disabled (LDO_EN_2=0x%02X)", ldo2_off);
    }

    /* teardown() will also ensure ALDO3 is off. */
    return pass;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

esp_err_t pmic_run_tests(pmic_handle_t h)
{
    bool pass = true;
    pass &= run_test(h, test_chip_id);
    pass &= run_test(h, test_register_probe);
    pass &= run_test(h, test_write_readback);
    pass &= run_test(h, test_aldo3_power_cycle);

    if (pass)
        ESP_LOGI(TAG, "=== PMIC tests: ALL PASS ===");
    else
        ESP_LOGE(TAG, "=== PMIC tests: FAILED ===");

    return pass ? ESP_OK : ESP_FAIL;
}

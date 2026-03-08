/*
 * tests.c — diagnostic / exploratory test suite
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.  Each phase adds its own
 * test function here (pmic_run_tests, epd_run_tests, rtc_run_tests, …).
 * tests_run() calls them in sequence and reports an overall pass/fail.
 *
 * To enable: idf.py menuconfig → Picture Frame → Test mode
 * Or add CONFIG_TEST_MODE=y to sdkconfig.defaults before building.
 *
 * Suite lifecycle
 * --------------
 * Each component test suite is bracketed by a setup and teardown function
 * defined in this file.  Setup/teardown operate at the cross-component level:
 * they configure ALL hardware that the suite depends on, not just the
 * component under test.  This prevents one suite from silently inheriting
 * ambiguous state left by the previous one.
 *
 * The pattern for each suite:
 *   suite_setup_*()   — log full PMIC register state so we can see exactly
 *                        what hardware state the suite starts in; then
 *                        explicitly configure every component the suite needs
 *   component_run_tests()
 *   suite_teardown_*() — shut down components in the correct order; log
 *                         PMIC state again so the next setup starts with
 *                         a documented baseline
 *
 * Per-test setup/teardown (within each component's own test file) handles
 * the intra-suite level: ensuring a known state between individual tests.
 */

#include "tests.h"
#include "pmic.h"
#include "epd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tests";

/* ── PMIC suite ──────────────────────────────────────────────────────────── */

static void pmic_suite_setup(pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "======== PMIC suite setup ========");
    /*
     * Log the full PMIC register state so we know exactly what the chip
     * looks like entering this suite — useful for spotting boot-time
     * defaults that differ from expectations.
     */
    pmic_log_state(pmic);

    /*
     * Establish known pre-suite state.  EPD power must be off: the PMIC
     * tests power-cycle ALDO3, and we need a defined starting point.
     */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));
    ESP_LOGI(TAG, "==================================");
}

static void pmic_suite_teardown(pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "======== PMIC suite teardown ========");
    /* Ensure ALDO3 is off regardless of how the last test ended. */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));
    /*
     * Log state again so the EPD suite setup starts with a documented
     * baseline — any unexpected register values are visible here.
     */
    pmic_log_state(pmic);
    ESP_LOGI(TAG, "=====================================");
}

/* ── EPD suite ───────────────────────────────────────────────────────────── */

static epd_handle_t epd_suite_setup(pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "======== EPD suite setup ========");
    /*
     * Log the full PMIC state at the suite boundary.  The EPD needs ALDO3
     * (EPD_VCC) on at 3.3 V; ALDO4 must also be at 3.3 V (set by
     * pmic_init()).  Seeing the register dump here confirms both are correct
     * and that nothing the PMIC tests did has leaked through.
     */
    pmic_log_state(pmic);

    /* Explicitly turn on EPD power regardless of PMIC test teardown state. */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, true));
    vTaskDelay(pdMS_TO_TICKS(50)); /* allow ALDO3 rail to stabilise */

    epd_handle_t epd;
    ESP_ERROR_CHECK(epd_init(&epd));

    ESP_LOGI(TAG, "=================================");
    return epd;
}

static void epd_suite_teardown(epd_handle_t epd, pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "======== EPD suite teardown ========");
    ESP_ERROR_CHECK(epd_sleep(epd));
    epd_deinit(epd);
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));
    /* Log final PMIC state — baseline for any future suite added here. */
    pmic_log_state(pmic);
    ESP_LOGI(TAG, "====================================");
}

/* ── Top-level orchestration ─────────────────────────────────────────────── */

void tests_run(pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test mode");
    ESP_LOGI(TAG, "========================================");

    int passed = 0, failed = 0;

    /* ── PMIC suite ── */
    pmic_suite_setup(pmic);
    if (pmic_run_tests(pmic) == ESP_OK)
        passed++;
    else
        failed++;
    pmic_suite_teardown(pmic);

    /* ── EPD suite ── */
    epd_handle_t epd = epd_suite_setup(pmic);
    if (epd_run_tests(epd) == ESP_OK)
        passed++;
    else
        failed++;
    epd_suite_teardown(epd, pmic);

    /*
     * Future suites follow the same pattern:
     *   rtc_suite_setup / rtc_run_tests / rtc_suite_teardown   — Phase 5
     *   img_suite_setup / img_run_tests / img_suite_teardown   — Phase 4
     */

    ESP_LOGI(TAG, "========================================");
    if (failed == 0)
        ESP_LOGI(TAG, " All %d test suite(s) PASSED", passed);
    else
        ESP_LOGE(TAG, " %d PASSED, %d FAILED", passed, failed);
    ESP_LOGI(TAG, "========================================");
}

void tests_main(pmic_handle_t pmic, i2c_master_bus_handle_t bus)
{
    tests_run(pmic);

    pmic_deinit(pmic);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
}

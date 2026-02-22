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
 * After all tests, the function returns to app_main() which halts.  The
 * device does NOT enter deep sleep in test mode — connect a serial monitor
 * to observe results.
 */

#include "tests.h"
#include "pmic.h"
#include "epd.h"
#include "esp_log.h"

static const char *TAG = "tests";

void tests_run(pmic_handle_t pmic, epd_handle_t epd)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test mode");
    ESP_LOGI(TAG, "========================================");

    int passed = 0, failed = 0;

    /* ── PMIC ── */
    if (pmic_run_tests(pmic) == ESP_OK)
        passed++;
    else
        failed++;

    /* ── EPD ── */
    if (epd_run_tests(epd) == ESP_OK)
        passed++;
    else
        failed++;

    /*
     * Future phases add their test calls here:
     *   rtc_run_tests(rtc)    — Phase 5
     *   img_run_tests()       — Phase 4
     */

    ESP_LOGI(TAG, "========================================");
    if (failed == 0)
        ESP_LOGI(TAG, " All %d test suite(s) PASSED", passed);
    else
        ESP_LOGE(TAG, " %d PASSED, %d FAILED", passed, failed);
    ESP_LOGI(TAG, "========================================");
}

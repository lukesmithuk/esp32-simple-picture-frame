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
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tests";

void tests_main(pmic_handle_t pmic, i2c_master_bus_handle_t bus)
{
    tests_run(pmic);

    /*
     * Orderly teardown: tests_run() has already powered off the EPD and
     * freed its handles.  Release pmic and I2C bus last.
     * I2C bus handle must be freed after all device handles that use it.
     */
    pmic_deinit(pmic);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
}

void tests_run(pmic_handle_t pmic)
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
    /*
     * EPD is initialised here — after PMIC tests — not before.
     *
     * pmic_run_tests() (test 4) power-cycles ALDO3 (the EPD supply rail).
     * If epd_init() were called before pmic_run_tests(), the panel would
     * lose power mid-session and end up in an uninitialised state; every
     * subsequent SPI command would be silently ignored and every BUSY wait
     * would time out.  Initialising the panel here, with the ALDO3 rail
     * stable, avoids this entirely.
     *
     * Allow 50 ms for the rail to settle before starting the init sequence.
     */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, true));
    vTaskDelay(pdMS_TO_TICKS(50));
    epd_handle_t epd;
    ESP_ERROR_CHECK(epd_init(&epd));

    if (epd_run_tests(epd) == ESP_OK)
        passed++;
    else
        failed++;

    /* Orderly EPD teardown before PMIC tests might alter rails further */
    ESP_ERROR_CHECK(epd_sleep(epd));
    epd_deinit(epd);
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));

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

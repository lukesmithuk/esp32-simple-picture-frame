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
    /*
     * EPD power comes from TG28 ALDO3 (I2C-controlled LDO) — there is no
     * dedicated EPD power GPIO.  The rail must be stable before the SPI
     * bus is touched, so a short settle delay follows the enable call.
     */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, true));
    vTaskDelay(pdMS_TO_TICKS(2));

    /*
     * epd_init() configures the SPI bus and GPIO lines, hardware-resets
     * the panel, and runs the full init sequence ending with POWER_ON.
     * The panel is ready to accept pixel data after this returns.
     */
    epd_handle_t epd;
    ESP_ERROR_CHECK(epd_init(&epd));

    tests_run(pmic, epd);

    /*
     * Orderly teardown: put the panel into deep sleep before cutting its
     * power rail, then release all driver handles.  Order matters — the
     * EPD SPI driver must be freed before pmic_deinit releases the I2C
     * device handle, and the I2C bus handle must be the last thing freed.
     */
    ESP_ERROR_CHECK(epd_sleep(epd));
    epd_deinit(epd);
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));
    pmic_deinit(pmic);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
}

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

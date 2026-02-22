/*
 * main.c — ESP32-S3 e-ink picture frame
 *
 * Boot-cycle model
 * ----------------
 * This firmware does NOT use a traditional main loop.  Each "iteration" is a
 * complete cold-boot → work → deep-sleep cycle:
 *
 *   boot → app_main() → init peripherals
 *                      → check if display update is due
 *                      → (if yes) power EPD, decode image, push frame
 *                      → power down peripherals
 *                      → pmic_sleep()
 *                      → esp_deep_sleep_start()   ← never returns
 *              ↑                                          |
 *              └──── PCF85063 RTC alarm → GPIO6 wakeup ──┘
 *                    (cold boot; app_main runs again from the top)
 *
 * Deep sleep keeps the ESP32-S3 RTC domain alive (~8 µA).  The PCF85063 RTC
 * fires an alarm on GPIO6 (active LOW, EXT0/EXT1 wakeup source) once per day
 * (or at a configured interval).  On wakeup the chip cold-boots; there is no
 * resume state.  Persistent data (image index, last-display timestamp) lives
 * in RTC fast memory (survives deep sleep) or NVS.
 *
 * Peripheral sharing
 * ------------------
 * All I2C devices (TG28 PMIC at 0x34, PCF85063 RTC at 0x51, SHTC3 at 0x70)
 * share a single i2c_master_bus_handle_t created here in app_main and passed
 * to each driver's init function.  Each driver adds its own device handle via
 * i2c_master_bus_add_device().
 *
 * Phase status (update as phases complete)
 * ----------------------------------------
 *   Phase 1 — Hardware bring-up       COMPLETE (2026-02-21)
 *   Phase 2 — PMIC driver             IN PROGRESS
 *   Phase 3 — EPD driver              pending
 *   Phase 4 — Image pipeline          pending
 *   Phase 5 — RTC / wake-sleep        pending
 *   Phase 6 — WiFi image fetch        deferred
 *   Phase 7 — Power optimisation      deferred
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pmic.h"

#ifdef CONFIG_TEST_MODE
#include "tests.h"
#endif

static const char *TAG = "main";

/* ── Pin assignments ─────────────────────────────────────────────────────── */

#define I2C_SDA_GPIO    47
#define I2C_SCL_GPIO    48
#define I2C_FREQ_HZ     100000

/*
 * RTC_INT_GPIO — PCF85063 interrupt, routed directly to GPIO6 on this board.
 * Active LOW: the PCF85063 pulls this line low when an alarm fires.
 * Used as the EXT0/EXT1 deep-sleep wakeup source.
 * (Not routed through the TG28 — confirmed from schematic 2026-02-21.)
 */
#define RTC_INT_GPIO    GPIO_NUM_6

/* ── I2C bus ─────────────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = I2C_SDA_GPIO,
        .scl_io_num           = I2C_SCL_GPIO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

/* ── Wakeup source ───────────────────────────────────────────────────────── */

static void configure_wakeup(void)
{
    /*
     * Configure EXT0 wakeup on GPIO6 (PCF85063 INT, active LOW).
     *
     * The PCF85063 holds the INT line LOW until the alarm flag is cleared.
     * We configure wakeup on the LOW level so the chip wakes as soon as the
     * alarm fires.  The alarm flag must be cleared early in app_main() on the
     * next boot to prevent an immediate re-wakeup.
     *
     * Phase 5 will add the alarm-flag clear and full RTC alarm programming.
     * For now this stub just registers the wakeup source.
     */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(RTC_INT_GPIO, 0 /* active LOW */));
    ESP_LOGI(TAG, "wakeup: EXT0 on GPIO%d (PCF85063 INT, active LOW)", RTC_INT_GPIO);
}

/* ── Update decision ─────────────────────────────────────────────────────── */

static bool update_is_due(void)
{
    /*
     * Phase 5 will implement proper logic:
     *   - Read wakeup cause (esp_sleep_get_wakeup_cause())
     *   - Check RTC alarm flag
     *   - Compare current time to last-display timestamp in RTC fast memory
     *
     * For now, always update so every boot exercises the display pipeline.
     */
    return true;
}

/* ── Display update ──────────────────────────────────────────────────────── */

static void do_display_update(pmic_handle_t pmic)
{
    ESP_LOGI(TAG, "display update begin");

    /* Power on the EPD (TG28 ALDO3 → EPD_VCC, 3.3 V) */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, true));
    vTaskDelay(pdMS_TO_TICKS(2)); /* allow rail to settle before SPI */

    /*
     * Phase 3: EPD driver
     *   epd_init();
     *   epd_display(framebuffer);
     *   epd_sleep();
     *
     * Phase 4: image pipeline
     *   img_load_next("/sdcard/images", framebuffer);
     */
    ESP_LOGW(TAG, "EPD and image pipeline not yet implemented (Phases 3–4)");

    /* Power off the EPD — must be done before deep sleep */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));

    ESP_LOGI(TAG, "display update end");
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /*
     * This function runs once per boot cycle and never loops.
     * See the boot-cycle model at the top of this file.
     */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Picture frame boot");
    ESP_LOGI(TAG, " Wakeup cause: %d", (int)esp_sleep_get_wakeup_cause());
    ESP_LOGI(TAG, "========================================");

    /* Shared I2C bus — passed to each peripheral driver */
    i2c_master_bus_handle_t bus = i2c_bus_init();

    /* PMIC — must be initialised first; controls EPD power and sleep rails */
    pmic_handle_t pmic;
    ESP_ERROR_CHECK(pmic_init(bus, &pmic));

    /*
     * Phase 5: RTC
     *   rtc_handle_t rtc;
     *   ESP_ERROR_CHECK(rtc_init(bus, &rtc));
     *   rtc_clear_alarm_flag(rtc);   // must clear before re-arming
     */

#ifdef CONFIG_TEST_MODE
    /*
     * Test mode: run diagnostics and return.  The device does NOT enter deep
     * sleep — keep a serial monitor connected to observe results.
     */
    tests_run(pmic);
    pmic_deinit(pmic);
    i2c_del_master_bus(bus);
    ESP_LOGI(TAG, "Test mode complete — halted (reset to run again)");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

    /* Production boot */
    if (update_is_due()) {
        do_display_update(pmic);
    } else {
        ESP_LOGI(TAG, "no update due; skipping display cycle");
    }

    /*
     * Phase 5: set next RTC alarm before sleeping
     *   rtc_set_alarm_tomorrow_8am(rtc);
     *   rtc_deinit(rtc);
     */

    /*
     * Prepare for deep sleep:
     *  1. Disable LDO rails via PMIC (ALDO3 and others off; DC1 stays on)
     *  2. Configure EXT0 wakeup on PCF85063 INT (GPIO6, active LOW)
     *  3. Release software handles (optional — reset clears them anyway)
     *  4. Enter deep sleep — this call never returns
     *
     * On wakeup: PCF85063 drives GPIO6 LOW → chip cold-boots → app_main()
     * runs again from the top.
     */
    ESP_ERROR_CHECK(pmic_sleep(pmic));
    pmic_deinit(pmic);
    i2c_del_master_bus(bus);

    configure_wakeup();

    ESP_LOGI(TAG, "entering deep sleep — goodbye");
    esp_deep_sleep_start();

    /* Never reached */
}

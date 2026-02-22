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
 *   Phase 2 — PMIC driver             COMPLETE (2026-02-22)
 *   Phase 3 — EPD driver              COMPLETE (2026-02-22)
 *   Phase 4 — Image pipeline          pending
 *   Phase 5 — RTC / wake-sleep        pending
 *   Phase 6 — WiFi image fetch        deferred
 *   Phase 7 — Power optimisation      deferred
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include "pmic.h"
#include "epd.h"

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

/* ── Deep sleep ──────────────────────────────────────────────────────────── */

/*
 * enter_deep_sleep() — shut down peripherals and enter deep sleep.
 *
 * Call order matters:
 *  1. pmic_sleep()          — disable LDO rails (ALDO3/EPD_VCC and others).
 *                             NOTE: zeroes LDO_EN_3 which disables DLDO1/DLDO2;
 *                             their rail mapping is unknown (ADR-014).  Safe in
 *                             production because the chip is about to power down,
 *                             but do NOT call this before any code that needs
 *                             serial output — it silences the USB-JTAG interface.
 *  2. pmic_deinit()         — release I2C device handle.
 *  3. i2c_del_master_bus()  — release I2C bus handle.
 *  4. EXT0 wakeup config    — GPIO6 (PCF85063 INT, active LOW).
 *  5. esp_deep_sleep_start() — never returns.
 *
 * On wakeup: PCF85063 drives GPIO6 LOW → cold boot → app_main() from top.
 */
static void enter_deep_sleep(pmic_handle_t pmic, i2c_master_bus_handle_t bus)
{
    ESP_ERROR_CHECK(pmic_sleep(pmic));
    pmic_deinit(pmic);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));

    /*
     * Configure EXT0 wakeup on GPIO6 (PCF85063 INT, active LOW).
     *
     * The PCF85063 holds INT low until the alarm flag is cleared in the next
     * boot's app_main().  Phase 5 will add alarm programming and flag clear.
     */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(RTC_INT_GPIO, 0 /* active LOW */));
    ESP_LOGI(TAG, "wakeup configured: EXT0 GPIO%d (PCF85063 INT, active LOW)",
             RTC_INT_GPIO);

    ESP_LOGI(TAG, "entering deep sleep — goodbye");
    esp_deep_sleep_start();
    /* Never reached */
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

    epd_handle_t epd;
    ESP_ERROR_CHECK(epd_init(&epd));

    /*
     * Framebuffer: 192 KB, 4bpp packed, allocated in PSRAM.
     *
     * Phase 4 will replace the white placeholder below with a JPEG loaded
     * from SD card and dithered to the Spectra 6 palette.
     */
    uint8_t *fb = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM);
    if (fb) {
        /* Solid white placeholder — index 1 packed into both nibbles */
        memset(fb, (EPD_COLOR_WHITE << 4) | EPD_COLOR_WHITE, EPD_FB_SIZE);
        ESP_LOGW(TAG, "Phase 4 not yet implemented — displaying white placeholder");

        /* Phase 4: replace above two lines with:
         *   img_load_next("/sdcard/images", fb, EPD_FB_SIZE);
         */

        ESP_ERROR_CHECK(epd_display(epd, fb, EPD_FB_SIZE));
        heap_caps_free(fb);
    } else {
        ESP_LOGE(TAG, "framebuffer alloc failed — skipping display");
    }

    ESP_ERROR_CHECK(epd_sleep(epd));
    epd_deinit(epd);

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
    ESP_LOGI(TAG, " Wakeup cause: 0x%02lx", esp_sleep_get_wakeup_causes());
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
     *
     * EPD must be powered on before initialising the driver.
     */
    ESP_ERROR_CHECK(pmic_epd_power(pmic, true));
    vTaskDelay(pdMS_TO_TICKS(2));

    epd_handle_t epd;
    ESP_ERROR_CHECK(epd_init(&epd));

    tests_run(pmic, epd);

    ESP_ERROR_CHECK(epd_sleep(epd));
    epd_deinit(epd);
    ESP_ERROR_CHECK(pmic_epd_power(pmic, false));
    pmic_deinit(pmic);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
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
     * TEMPORARY: halt before enter_deep_sleep() so the USB-JTAG serial
     * interface stays active for debugging.  See ADR-014: pmic_sleep() zeros
     * LDO_EN_3 (DLDO1/DLDO2), which silences serial output.  Reinstate
     * enter_deep_sleep() once DLDO rail mapping is confirmed (Phase 7).
     */
    ESP_LOGI(TAG, "halting before sleep (debug build) — reset to run again");
    for (int i = 0; ; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "halted [%d]", i);
    }

    /* Phase 5: set next RTC alarm before sleeping
     *   rtc_set_alarm_tomorrow_8am(rtc);
     *   rtc_deinit(rtc);
     */

    enter_deep_sleep(pmic, bus);
}

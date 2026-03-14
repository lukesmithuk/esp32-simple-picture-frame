#include <string.h>
#include "sdcard.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sdcard";

/* GPIOs — Waveshare ESP32-S3-PhotoPainter schematic */
#define SDCARD_PIN_CLK  GPIO_NUM_39
#define SDCARD_PIN_CMD  GPIO_NUM_41
#define SDCARD_PIN_D0   GPIO_NUM_40
#define SDCARD_PIN_D1   GPIO_NUM_1
#define SDCARD_PIN_D2   GPIO_NUM_2
#define SDCARD_PIN_D3   GPIO_NUM_38

static sdmmc_card_t *s_card = NULL;

esp_err_t sdcard_mount(void)
{
    if (s_card) {
        ESP_LOGD(TAG, "Already mounted");
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0  = SDCARD_PIN_D0;
    slot_config.d1  = SDCARD_PIN_D1;
    slot_config.d2  = SDCARD_PIN_D2;
    slot_config.d3  = SDCARD_PIN_D3;

    ESP_LOGI(TAG, "Mounting SD card (4-bit SDIO)");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        s_card = NULL;
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at " SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (!s_card) {
        return;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD unmount error: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD card unmounted");
    }
    s_card = NULL;
}

bool sdcard_is_mounted(void)
{
    return s_card != NULL;
}

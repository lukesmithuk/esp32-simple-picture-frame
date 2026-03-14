#include <stdio.h>
#include "image_loader.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "image_loader";

esp_err_t image_loader_load(const char *path,
                            uint8_t **out_buf, size_t *out_size)
{
    *out_buf = NULL;
    *out_size = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Empty or unreadable: %s", path);
        fclose(f);
        return ESP_FAIL;
    }

    if ((size_t)file_size > IMAGE_LOADER_MAX_FILE_BYTES) {
        ESP_LOGE(TAG, "File too large: %ld bytes (max %d)", file_size,
                 IMAGE_LOADER_MAX_FILE_BYTES);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *buf = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %ld bytes", file_size);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (read != (size_t)file_size) {
        ESP_LOGE(TAG, "Short read: got %zu of %ld bytes", read, file_size);
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loaded %ld bytes from %s", file_size, path);
    *out_buf = buf;
    *out_size = (size_t)file_size;
    return ESP_OK;
}

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "image_picker.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "image_picker";

static bool ext_matches(const char *filename, const char *const *exts)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return false;
    }
    const char *ext = dot + 1;
    for (const char *const *e = exts; *e; e++) {
        if (strcasecmp(ext, *e) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_include(const struct dirent *entry, const char *const *exts)
{
    if (entry->d_type != DT_REG) {
        return false;
    }
    /* Skip macOS resource fork files */
    if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
        return false;
    }
    return ext_matches(entry->d_name, exts);
}

esp_err_t image_picker_pick(const char *dir_path,
                            const char *const *exts,
                            char *out_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open directory: %s", dir_path);
        return ESP_FAIL;
    }

    /* First pass: count matching files */
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (should_include(entry, exts)) {
            count++;
        }
    }

    if (count == 0) {
        closedir(dir);
        ESP_LOGW(TAG, "No matching images in %s", dir_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Pick a random index */
    int target = (int)(esp_random() % (uint32_t)count);
    ESP_LOGD(TAG, "Found %d images, selected index %d", count, target);

    /* Second pass: find the target file */
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!should_include(entry, exts)) {
            continue;
        }
        if (idx == target) {
            int n = snprintf(out_path, IMAGE_PICKER_PATH_MAX,
                             "%s/%s", dir_path, entry->d_name);
            if (n >= IMAGE_PICKER_PATH_MAX) {
                ESP_LOGW(TAG, "Path truncated: %s/%s", dir_path, entry->d_name);
            }
            ESP_LOGI(TAG, "Selected: %s", out_path);
            closedir(dir);
            return ESP_OK;
        }
        idx++;
    }

    /* Should not reach here */
    closedir(dir);
    ESP_LOGE(TAG, "Failed to locate selected file (index %d)", target);
    return ESP_FAIL;
}

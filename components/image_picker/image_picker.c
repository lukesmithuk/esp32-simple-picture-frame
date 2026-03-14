#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "image_picker.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#define HISTORY_FILENAME ".image_history"
#define MAX_IMAGES       256
#define MAX_NAME_LEN     128

static const char *TAG = "image_picker";

/* ── Helpers ─────────────────────────────────────────────────────────────── */

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
    /* Skip hidden files (including our history file) */
    if (entry->d_name[0] == '.') {
        return false;
    }
    return ext_matches(entry->d_name, exts);
}

/* ── History file I/O ────────────────────────────────────────────────────── */

/**
 * Read history file into an array of filenames (names only, no path).
 * Returns the number of entries read.
 */
static int read_history(const char *history_path, char history[][MAX_NAME_LEN],
                        int max_entries)
{
    FILE *f = fopen(history_path, "r");
    if (!f)
        return 0;

    int count = 0;
    char line[MAX_NAME_LEN];
    while (count < max_entries && fgets(line, sizeof(line), f)) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] == '\0')
            continue;
        strncpy(history[count], line, MAX_NAME_LEN - 1);
        history[count][MAX_NAME_LEN - 1] = '\0';
        count++;
    }

    fclose(f);
    return count;
}

static bool is_in_history(const char *name, const char history[][MAX_NAME_LEN],
                          int history_count)
{
    for (int i = 0; i < history_count; i++) {
        if (strcmp(name, history[i]) == 0)
            return true;
    }
    return false;
}

static void write_history(const char *history_path,
                          const char history[][MAX_NAME_LEN],
                          int history_count, const char *new_entry)
{
    FILE *f = fopen(history_path, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot write history file: %s", history_path);
        return;
    }

    /* Write existing entries (already pruned to current files). */
    for (int i = 0; i < history_count; i++) {
        fprintf(f, "%s\n", history[i]);
    }
    fprintf(f, "%s\n", new_entry);

    fclose(f);
}

static void clear_history(const char *history_path)
{
    remove(history_path);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Name list: a single PSRAM block holding up to MAX_IMAGES names. */
typedef char name_list_t[MAX_IMAGES][MAX_NAME_LEN];

esp_err_t image_picker_pick(const char *dir_path,
                            const char *const *exts,
                            char *out_path)
{
    /* Allocate working buffers from PSRAM (3 × 32 KB = 96 KB).
     * TODO: candidates could be eliminated with a two-pass count+seek approach. */
    name_list_t *files      = heap_caps_malloc(sizeof(name_list_t), MALLOC_CAP_SPIRAM);
    name_list_t *history    = heap_caps_malloc(sizeof(name_list_t), MALLOC_CAP_SPIRAM);
    name_list_t *candidates = heap_caps_malloc(sizeof(name_list_t), MALLOC_CAP_SPIRAM);
    if (!files || !history || !candidates) {
        ESP_LOGE(TAG, "Failed to alloc picker buffers");
        free(files);
        free(history);
        free(candidates);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;

    /* Build history file path. */
    char history_path[IMAGE_PICKER_PATH_MAX];
    snprintf(history_path, sizeof(history_path), "%s/%s",
             dir_path, HISTORY_FILENAME);

    /* Scan directory for matching files (names only). */
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open directory: %s", dir_path);
        goto cleanup;
    }

    int file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < MAX_IMAGES) {
        if (should_include(entry, exts)) {
            strncpy((*files)[file_count], entry->d_name, MAX_NAME_LEN - 1);
            (*files)[file_count][MAX_NAME_LEN - 1] = '\0';
            file_count++;
        }
    }
    closedir(dir);

    if (file_count == 0) {
        ESP_LOGW(TAG, "No matching images in %s", dir_path);
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Found %d images", file_count);

    /* Read history and prune entries for files that no longer exist. */
    int history_count = read_history(history_path, *history, MAX_IMAGES);
    ESP_LOGI(TAG, "History has %d entries:", history_count);
    for (int i = 0; i < history_count; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, (*history)[i]);
    }

    int pruned_count = 0;
    for (int i = 0; i < history_count; i++) {
        bool exists = false;
        for (int j = 0; j < file_count; j++) {
            if (strcmp((*history)[i], (*files)[j]) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) {
            if (pruned_count != i)
                strncpy((*history)[pruned_count], (*history)[i], MAX_NAME_LEN);
            pruned_count++;
        }
    }
    history_count = pruned_count;

    /* Build candidate list: files NOT in history. */
    int candidate_count = 0;
    for (int i = 0; i < file_count; i++) {
        if (!is_in_history((*files)[i], *history, history_count)) {
            strncpy((*candidates)[candidate_count], (*files)[i], MAX_NAME_LEN);
            candidate_count++;
        }
    }

    /* If all files have been shown, reset history and use full list. */
    if (candidate_count == 0) {
        ESP_LOGI(TAG, "All %d images shown, resetting history", file_count);
        clear_history(history_path);
        memcpy(*candidates, *files, file_count * MAX_NAME_LEN);
        candidate_count = file_count;
        history_count = 0;
    }

    /* Pick randomly from candidates. */
    int target = (int)(esp_random() % (uint32_t)candidate_count);
    const char *chosen = (*candidates)[target];

    ESP_LOGI(TAG, "Selected: %s (%d candidates, %d in history)",
             chosen, candidate_count, history_count);

    /* Build full path. */
    int n = snprintf(out_path, IMAGE_PICKER_PATH_MAX,
                     "%s/%s", dir_path, chosen);
    if (n >= IMAGE_PICKER_PATH_MAX) {
        ESP_LOGW(TAG, "Path truncated: %s/%s", dir_path, chosen);
    }

    /* Append to history. */
    write_history(history_path, *history, history_count, chosen);

    result = ESP_OK;

cleanup:
    free(files);
    free(history);
    free(candidates);
    return result;
}

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#define MAX_ENTRIES  32
#define MAX_KEY_LEN  32
#define MAX_VAL_LEN  128
#define MAX_LINE_LEN (MAX_KEY_LEN + MAX_VAL_LEN + 2)

static const char *TAG = "config";

static struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
} s_entries[MAX_ENTRIES];
static int s_count = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    if (len == 0)
        return s;
    char *end = s + len - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static const char *find_value(const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].value;
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t config_load(const char *path)
{
    s_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file at %s — using defaults", path);
        return ESP_OK;
    }

    char line[MAX_LINE_LEN];
    int line_num = 0;
    while (fgets(line, sizeof(line), f) && s_count < MAX_ENTRIES) {
        line_num++;

        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            ESP_LOGW(TAG, "Skipping line %d: no '=' found", line_num);
            continue;
        }

        *eq = '\0';
        char *key = trim(trimmed);
        char *val = trim(eq + 1);

        strncpy(s_entries[s_count].key, key, MAX_KEY_LEN - 1);
        s_entries[s_count].key[MAX_KEY_LEN - 1] = '\0';
        strncpy(s_entries[s_count].value, val, MAX_VAL_LEN - 1);
        s_entries[s_count].value[MAX_VAL_LEN - 1] = '\0';

        /* Mask sensitive values in log output. */
        bool sensitive = (strcasestr(key, "password") != NULL
                       || strcasestr(key, "key") != NULL);
        ESP_LOGI(TAG, "%s = %s", s_entries[s_count].key,
                 sensitive ? "****" : s_entries[s_count].value);
        s_count++;
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d config entries from %s", s_count, path);
    return ESP_OK;
}

int config_get_int(const char *key, int default_value)
{
    const char *val = find_value(key);
    if (!val)
        return default_value;
    return atoi(val);
}

const char *config_get_str(const char *key, const char *default_value)
{
    const char *val = find_value(key);
    return val ? val : default_value;
}

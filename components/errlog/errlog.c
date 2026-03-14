#include <stdio.h>
#include <time.h>
#include "errlog.h"
#include "esp_log.h"

static const char *TAG = "errlog";

esp_err_t errlog_write(const char *log_path, const char *message)
{
    FILE *f = fopen(log_path, "a");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open log file: %s", log_path);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (t.tm_year + 1900 >= 2020) {
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec,
                message);
    } else {
        fprintf(f, "[NO-RTC] %s\n", message);
    }

    fclose(f);
    ESP_LOGD(TAG, "Logged to %s: %s", log_path, message);
    return ESP_OK;
}

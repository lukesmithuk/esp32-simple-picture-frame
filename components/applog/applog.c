#include "applog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#define EARLY_BUF_SIZE 4096

static const char *TAG = "applog";

/* ── Early boot buffer ───────────────────────────────────────────────────── */

static char s_early_buf[EARLY_BUF_SIZE];
static size_t s_early_len = 0;
static bool s_buffering = false;

/* ── File logging state ──────────────────────────────────────────────────── */

static FILE *s_log_file = NULL;
static vprintf_like_t s_original_vprintf = NULL;

/* ── vprintf handlers ────────────────────────────────────────────────────── */

static int log_to_buffer_and_serial(const char *fmt, va_list args)
{
    /* Append to early buffer if space remains. */
    if (s_early_len < EARLY_BUF_SIZE) {
        va_list args_copy;
        va_copy(args_copy, args);
        int n = vsnprintf(s_early_buf + s_early_len,
                          EARLY_BUF_SIZE - s_early_len, fmt, args_copy);
        va_end(args_copy);
        if (n > 0) {
            size_t written = (size_t)n;
            if (s_early_len + written > EARLY_BUF_SIZE)
                written = EARLY_BUF_SIZE - s_early_len;
            s_early_len += written;
        }
    }

    /* Write to serial via the original handler. */
    return s_original_vprintf(fmt, args);
}

static int log_to_file_and_serial(const char *fmt, va_list args)
{
    if (s_log_file) {
        va_list args_copy;
        va_copy(args_copy, args);
        vfprintf(s_log_file, fmt, args_copy);
        va_end(args_copy);
    }

    return s_original_vprintf(fmt, args);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void applog_init(void)
{
    s_early_len = 0;
    s_buffering = true;
    s_original_vprintf = esp_log_set_vprintf(log_to_buffer_and_serial);
}

esp_err_t applog_start(const char *log_path)
{
    if (s_log_file) {
        ESP_LOGW(TAG, "Log capture already active");
        return ESP_OK;
    }

    s_log_file = fopen(log_path, "a");
    if (!s_log_file) {
        ESP_LOGW(TAG, "Cannot open log file: %s", log_path);
        return ESP_FAIL;
    }

    /* Flush any buffered early-boot messages to the file. */
    if (s_buffering && s_early_len > 0) {
        fwrite(s_early_buf, 1, s_early_len, s_log_file);
        s_early_len = 0;
    }
    s_buffering = false;

    /* Switch from buffer handler to file handler.  s_original_vprintf
     * was already captured by applog_init(). */
    esp_log_set_vprintf(log_to_file_and_serial);

    ESP_LOGI(TAG, "Log capture started → %s", log_path);
    return ESP_OK;
}

void applog_stop(void)
{
    if (!s_log_file && !s_buffering)
        return;

    ESP_LOGI(TAG, "Log capture stopped");

    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = NULL;
    }

    if (s_log_file) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }

    s_buffering = false;
    s_early_len = 0;
}

/* ── Explicit timestamped message ────────────────────────────────────────── */

esp_err_t applog_write(const char *log_path, const char *message)
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
    return ESP_OK;
}

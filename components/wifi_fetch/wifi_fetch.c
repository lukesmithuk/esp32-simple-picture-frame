#include "wifi_fetch.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECT_TIMEOUT_MS  10000
#define NTP_SYNC_TIMEOUT_MS      5000
#define LOG_UPLOAD_MAX_BYTES     (16 * 1024)
#define IMAGE_MAX_BYTES          (4 * 1024 * 1024)

static const char *TAG = "wifi_fetch";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static char s_mac_str[18];  /* "AA:BB:CC:DD:EE:FF\0" */

/* ── WiFi event handler ──────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void get_mac_string(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac_str, sizeof(s_mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void sync_ntp(void)
{
    ESP_LOGI(TAG, "Starting NTP sync");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    int elapsed = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED
           && elapsed < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
    }

    esp_sntp_stop();

    if (elapsed >= NTP_SYNC_TIMEOUT_MS) {
        ESP_LOGW(TAG, "NTP sync timed out after %d ms", NTP_SYNC_TIMEOUT_MS);
        return;
    }

    time_t now = time(NULL);
    board_rtc_set_time(now);
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "NTP sync OK — RTC set to %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

static void build_url(char *buf, size_t buf_size,
                      const char *server_url, const char *path)
{
    int n = snprintf(buf, buf_size, "%s%s", server_url, path);
    if (n >= (int)buf_size) {
        ESP_LOGW(TAG, "URL truncated (%d chars, buffer %zu)", n, buf_size);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t wifi_fetch_init(const char *ssid, const char *password)
{
    get_mac_string();
    ESP_LOGI(TAG, "Frame MAC: %s", s_mac_str);

    /* NVS is required by the WiFi driver for calibration data. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t el_err = esp_event_loop_create_default();
    if (el_err != ESP_OK && el_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(el_err));
        return el_err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connect failed/timed out");
        wifi_fetch_deinit();
        return ESP_ERR_TIMEOUT;
    }

    sync_ntp();
    return ESP_OK;
}

void wifi_fetch_deinit(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    esp_netif_deinit();
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    ESP_LOGI(TAG, "WiFi disconnected");
}

/* ── Image fetch ─────────────────────────────────────────────────────────── */

static char s_image_name[128];
static int s_wake_hours   = -1;
static int s_wake_minutes = -1;
static int s_wake_seconds = -1;

static esp_err_t image_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "X-Image-Name") == 0) {
            strncpy(s_image_name, evt->header_value, sizeof(s_image_name) - 1);
            s_image_name[sizeof(s_image_name) - 1] = '\0';
        } else if (strcasecmp(evt->header_key, "X-Wake-Hours") == 0) {
            s_wake_hours = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "X-Wake-Minutes") == 0) {
            s_wake_minutes = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "X-Wake-Seconds") == 0) {
            s_wake_seconds = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size)
{
    *out_buf = NULL;
    *out_size = 0;
    s_image_name[0] = '\0';
    s_wake_hours = -1;
    s_wake_minutes = -1;
    s_wake_seconds = -1;

    char url[256];
    build_url(url, sizeof(url), server_url, "/api/next");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .event_handler = image_http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "X-API-Key", api_key);
    esp_http_client_set_header(client, "X-Frame-ID", s_mac_str);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status == 204) {
        ESP_LOGI(TAG, "Server has no images (204)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Server returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length <= 0 || content_length > IMAGE_MAX_BYTES) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to alloc %d bytes for image", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, (char *)buf + total_read,
                                            content_length - total_read);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "HTTP read failed at %d/%d bytes",
                     total_read, content_length);
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total_read += read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_buf = buf;
    *out_size = total_read;
    ESP_LOGI(TAG, "Downloaded %d bytes from server: %s",
             total_read, s_image_name[0] ? s_image_name : "unknown");

    if (s_wake_hours >= 0)
        ESP_LOGI(TAG, "Server wake interval: %dh %dm %ds",
                 s_wake_hours, s_wake_minutes, s_wake_seconds);
    return ESP_OK;
}

bool wifi_fetch_get_wake_interval(int *hours, int *minutes, int *seconds)
{
    if (s_wake_hours < 0)
        return false;
    *hours   = s_wake_hours;
    *minutes = s_wake_minutes;
    *seconds = s_wake_seconds;
    return true;
}

/* ── Status push ─────────────────────────────────────────────────────────── */

esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key,
                                 const wifi_fetch_status_t *status)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(json, "battery_connected", status->battery_connected);
    cJSON_AddNumberToObject(json, "battery_percent", status->battery_percent);
    cJSON_AddNumberToObject(json, "battery_mv", status->battery_mv);
    cJSON_AddBoolToObject(json, "charging", status->charging);
    cJSON_AddBoolToObject(json, "usb_connected", status->usb_connected);
    cJSON_AddNumberToObject(json, "sd_free_kb", status->sd_free_kb);
    if (status->firmware_version)
        cJSON_AddStringToObject(json, "firmware_version", status->firmware_version);

    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;

    char url[256];
    build_url(url, sizeof(url), server_url, "/api/status");

    esp_http_client_config_t config = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-API-Key", api_key);
    esp_http_client_set_header(client, "X-Frame-ID", s_mac_str);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Status push failed (HTTP %d): %s",
                 status_code, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status pushed to server");
    return ESP_OK;
}

/* ── Log upload ──────────────────────────────────────────────────────────── */

esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path)
{
    /* Read stored offset. */
    long offset = 0;
    FILE *of = fopen(offset_path, "r");
    if (of) {
        fscanf(of, "%ld", &offset);
        fclose(of);
    }

    /* Check current log file size. */
    struct stat st;
    if (stat(log_path, &st) != 0) {
        ESP_LOGD(TAG, "Log file not found: %s", log_path);
        return ESP_OK;
    }

    /* Handle rotation: file shrank. */
    if (st.st_size < offset) {
        ESP_LOGI(TAG, "Log file rotated (size %ld < offset %ld), resetting",
                 st.st_size, offset);
        offset = 0;
    }

    long new_bytes = st.st_size - offset;
    if (new_bytes <= 0) {
        ESP_LOGD(TAG, "No new log bytes to upload");
        return ESP_OK;
    }

    if (new_bytes > LOG_UPLOAD_MAX_BYTES)
        new_bytes = LOG_UPLOAD_MAX_BYTES;

    FILE *lf = fopen(log_path, "r");
    if (!lf) return ESP_FAIL;
    fseek(lf, offset, SEEK_SET);

    char *buf = heap_caps_malloc(new_bytes, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(lf); return ESP_ERR_NO_MEM; }

    size_t read_len = fread(buf, 1, new_bytes, lf);
    fclose(lf);

    if (read_len == 0) {
        free(buf);
        return ESP_OK;
    }

    char url[256];
    build_url(url, sizeof(url), server_url, "/api/logs");

    esp_http_client_config_t config = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-API-Key", api_key);
    esp_http_client_set_header(client, "X-Frame-ID", s_mac_str);
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_post_field(client, buf, read_len);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(buf);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Log upload failed (HTTP %d)", status_code);
        return ESP_FAIL;
    }

    /* Update stored offset. */
    of = fopen(offset_path, "w");
    if (of) {
        fprintf(of, "%ld", offset + (long)read_len);
        fclose(of);
    }

    ESP_LOGI(TAG, "Uploaded %zu log bytes (offset %ld → %ld)",
             read_len, offset, offset + (long)read_len);
    return ESP_OK;
}

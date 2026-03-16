# WiFi Firmware — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WiFi connectivity to the ESP32-S3 picture frame firmware: fetch images from a photo server, push status/logs, sync RTC via NTP, with SD card fallback.

**Architecture:** New `wifi_fetch` component owns WiFi lifecycle and HTTP client. Modifications to `applog` (log rolling), `config` (sensitive key masking), and `main.c` (WiFi-first flow with SD fallback).

**Tech Stack:** ESP-IDF v5.5.3, `esp_wifi`, `esp_http_client`, `esp_sntp`, `esp_netif`

**Prerequisite:** The photo frame server must be running for integration testing. See `docs/superpowers/plans/2026-03-15-wifi-server.md`.

---

## File Structure

```
components/wifi_fetch/
  CMakeLists.txt                    # Component build config
  include/wifi_fetch.h              # Public API
  wifi_fetch.c                      # WiFi, NTP, HTTP implementation

Modified files:
  components/applog/applog.c        # Add log rolling
  components/config/config.c        # Mask sensitive keys in log output
  main/main.c                       # WiFi-first flow with SD fallback
  sdkconfig.defaults                # WiFi + SPIRAM WiFi lwIP settings
```

---

## Chunk 1: Config Masking + Log Rolling

### Task 1: Mask sensitive config keys

**Files:**
- Modify: `components/config/config.c`

- [ ] **Step 1: Modify config_load() to mask sensitive values**

In `config.c`, change the log line in `config_load()` from:

```c
ESP_LOGI(TAG, "%s = %s", s_entries[s_count].key,
         s_entries[s_count].value);
```

to:

```c
/* Mask values for keys containing "password" or "key". */
bool sensitive = (strcasestr(key, "password") != NULL
               || strcasestr(key, "key") != NULL);
ESP_LOGI(TAG, "%s = %s", s_entries[s_count].key,
         sensitive ? "****" : s_entries[s_count].value);
```

Add `#include <strings.h>` at the top for `strcasestr`.

- [ ] **Step 2: Build and verify**

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add components/config/config.c
git commit -m "feat(config): mask sensitive keys (password, key) in log output"
```

### Task 2: Add log rolling to applog

**Files:**
- Modify: `components/applog/applog.c`
- Modify: `components/applog/include/applog.h`

- [ ] **Step 1: Add max_size_kb parameter to applog_start()**

Update `applog.h` — change the signature:

```c
/**
 * @brief Start capturing all ESP_LOG output to a file.
 *
 * If the log file exceeds max_size_kb, it is rotated: renamed to
 * <path>.1 (overwriting any previous backup) and a fresh file started.
 *
 * @param log_path     Absolute path, e.g. "/sdcard/system.log".
 * @param max_size_kb  Maximum log file size before rotation (0 = no limit).
 * @return ESP_OK on success, ESP_FAIL if the file cannot be opened.
 */
esp_err_t applog_start(const char *log_path, int max_size_kb);
```

- [ ] **Step 2: Implement log rolling in applog.c**

At the start of `applog_start()`, after the early checks and before `fopen`:

```c
/* Roll the log file if it exceeds the size limit. */
if (max_size_kb > 0) {
    struct stat st;
    if (stat(log_path, &st) == 0 && st.st_size > max_size_kb * 1024) {
        char backup[256];
        snprintf(backup, sizeof(backup), "%s.1", log_path);
        remove(backup);
        rename(log_path, backup);
        ESP_LOGI(TAG, "Log rotated: %s → %s (%ld bytes)",
                 log_path, backup, st.st_size);
    }
}
```

Add `#include <sys/stat.h>` at the top.

- [ ] **Step 3: Update the call in main.c**

Change:

```c
applog_start(SYSTEM_LOG);
```

to:

```c
int log_max_kb = config_get_int("log_max_size_kb", 256);
applog_start(SYSTEM_LOG, log_max_kb);
```

This appears in two places in main.c (WiFi path and non-WiFi path).

- [ ] **Step 4: Build and verify**

```bash
idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add components/applog/ main/main.c
git commit -m "feat(applog): log rolling when file exceeds configurable max size"
```

---

## Chunk 2: wifi_fetch Component — WiFi + NTP

### Task 3: Component skeleton

**Files:**
- Create: `components/wifi_fetch/CMakeLists.txt`
- Create: `components/wifi_fetch/include/wifi_fetch.h`
- Create: `components/wifi_fetch/wifi_fetch.c` (stub)
- Modify: `main/CMakeLists.txt`
- Modify: `sdkconfig.defaults`

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "wifi_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_http_client
        esp_netif
        esp_hw_support
        esp_event
        board
        json
)
```

- [ ] **Step 2: Create wifi_fetch.h**

```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  battery_connected;
    int   battery_percent;
    int   battery_mv;
    bool  charging;
    bool  usb_connected;
    int   sd_free_kb;
    const char *firmware_version;
} wifi_fetch_status_t;

/**
 * @brief Connect to WiFi and sync time via NTP → RTC.
 *
 * Blocks up to 10 seconds for WiFi association, then up to 5 seconds
 * for NTP sync. RTC is updated on successful NTP sync.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if WiFi connect fails.
 */
esp_err_t wifi_fetch_init(const char *ssid, const char *password);

/**
 * @brief Fetch the next image from the server.
 *
 * GET /api/next with API key auth. Pre-allocates PSRAM buffer based
 * on Content-Length. Caller must free *out_buf.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if gallery empty (204),
 *         ESP_FAIL on error.
 */
esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size);

/**
 * @brief Push frame status to server as JSON.
 */
esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key,
                                 const wifi_fetch_status_t *status);

/**
 * @brief Push new log lines since last upload.
 *
 * Reads from log_path starting at the byte offset stored in offset_path.
 * Updates offset_path on success. Caps upload at 16 KB per call.
 *
 * @return ESP_OK on success, ESP_FAIL on error (non-fatal, retry next cycle).
 */
esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path);

/**
 * @brief Disconnect WiFi and release resources.
 */
void wifi_fetch_deinit(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create wifi_fetch.c stub**

```c
#include "wifi_fetch.h"
#include "esp_log.h"

static const char *TAG = "wifi_fetch";

esp_err_t wifi_fetch_init(const char *ssid, const char *password)
{
    ESP_LOGW(TAG, "wifi_fetch_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key,
                                 const wifi_fetch_status_t *status)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void wifi_fetch_deinit(void)
{
}
```

- [ ] **Step 4: Add wifi_fetch to main/CMakeLists.txt REQUIRES**

- [ ] **Step 5: Add WiFi + SPIRAM settings to sdkconfig.defaults**

```
# WiFi
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

- [ ] **Step 6: Delete build/ and sdkconfig to force regeneration, then build**

```bash
rm -rf build sdkconfig
idf.py build
```

- [ ] **Step 7: Commit**

```bash
git add components/wifi_fetch/ main/CMakeLists.txt sdkconfig.defaults
git commit -m "feat(wifi_fetch): component skeleton with stub API"
```

### Task 4: WiFi connect + NTP sync

**Files:**
- Modify: `components/wifi_fetch/wifi_fetch.c`

- [ ] **Step 1: Implement wifi_fetch_init()**

Replace the stub in `wifi_fetch.c`:

```c
#include "wifi_fetch.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
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

esp_err_t wifi_fetch_init(const char *ssid, const char *password)
{
    get_mac_string();
    ESP_LOGI(TAG, "Frame MAC: %s", s_mac_str);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

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
```

- [ ] **Step 2: Build**

```bash
idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add components/wifi_fetch/wifi_fetch.c
git commit -m "feat(wifi_fetch): WiFi connect with timeout + NTP → RTC sync"
```

---

## Chunk 3: HTTP Client — Image Fetch, Status, Logs

### Task 5: Implement wifi_fetch_image()

**Files:**
- Modify: `components/wifi_fetch/wifi_fetch.c`

- [ ] **Step 1: Add image fetch implementation**

```c
esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size)
{
    *out_buf = NULL;
    *out_size = 0;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/next", server_url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
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
        int read = esp_http_client_read(client, (char *)buf + total_read,
                                        content_length - total_read);
        if (read <= 0) {
            ESP_LOGE(TAG, "HTTP read failed at %d/%d bytes", total_read, content_length);
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total_read += read;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_buf = buf;
    *out_size = total_read;
    ESP_LOGI(TAG, "Downloaded %d bytes from server", total_read);
    return ESP_OK;
}
```

- [ ] **Step 2: Build**

```bash
idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add components/wifi_fetch/wifi_fetch.c
git commit -m "feat(wifi_fetch): image download with Content-Length pre-alloc"
```

### Task 6: Implement wifi_fetch_post_status()

**Files:**
- Modify: `components/wifi_fetch/wifi_fetch.c`

- [ ] **Step 1: Add status push implementation**

```c
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
    snprintf(url, sizeof(url), "%s/api/status", server_url);

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
        ESP_LOGW(TAG, "Status push failed (HTTP %d): %s", status_code, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status pushed to server");
    return ESP_OK;
}
```

- [ ] **Step 2: Build and commit**

```bash
idf.py build
git add components/wifi_fetch/wifi_fetch.c
git commit -m "feat(wifi_fetch): POST /api/status with JSON body"
```

### Task 7: Implement wifi_fetch_post_logs()

**Files:**
- Modify: `components/wifi_fetch/wifi_fetch.c`

- [ ] **Step 1: Add log upload implementation**

```c
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
        return ESP_OK;  /* Nothing to upload */
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

    /* Cap at LOG_UPLOAD_MAX_BYTES. */
    if (new_bytes > LOG_UPLOAD_MAX_BYTES)
        new_bytes = LOG_UPLOAD_MAX_BYTES;

    /* Read new bytes. */
    FILE *lf = fopen(log_path, "r");
    if (!lf) return ESP_FAIL;
    fseek(lf, offset, SEEK_SET);

    char *buf = malloc(new_bytes);
    if (!buf) { fclose(lf); return ESP_ERR_NO_MEM; }

    size_t read = fread(buf, 1, new_bytes, lf);
    fclose(lf);

    if (read == 0) {
        free(buf);
        return ESP_OK;
    }

    /* POST to server. */
    char url[256];
    snprintf(url, sizeof(url), "%s/api/logs", server_url);

    esp_http_client_config_t config = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-API-Key", api_key);
    esp_http_client_set_header(client, "X-Frame-ID", s_mac_str);
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_post_field(client, buf, read);

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
        fprintf(of, "%ld", offset + (long)read);
        fclose(of);
    }

    ESP_LOGI(TAG, "Uploaded %zu log bytes (offset %ld → %ld)",
             read, offset, offset + (long)read);
    return ESP_OK;
}
```

- [ ] **Step 2: Build and commit**

```bash
idf.py build
git add components/wifi_fetch/wifi_fetch.c
git commit -m "feat(wifi_fetch): incremental log upload with offset tracking"
```

---

## Chunk 4: Integration in main.c

### Task 8: Wire WiFi flow into app_main

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Add includes and defines**

Add to the includes in main.c:

```c
#include "wifi_fetch.h"
```

Add defines:

```c
#define LOG_OFFSET_PATH  SDCARD_MOUNT_POINT "/.log_offset"
#define FIRMWARE_VERSION "1.0.0"
```

- [ ] **Step 2: Restructure app_main production path**

Replace the production path (after `sd_mounted = true`) with the WiFi-first flow from the spec. The key changes:

1. Check for `wifi_ssid` in config
2. If present: init WiFi, upload logs, start applog (with rolling), push status, fetch image, deinit WiFi
3. If no WiFi or image fetch failed: fall back to SD card path
4. Rest of the pipeline (decode, display, sleep) unchanged

```c
    sd_mounted = true;

    /* Load config (wake interval, WiFi, etc.). Missing file uses defaults. */
    config_load(CONFIG_PATH);

    /* Try WiFi path first. */
    const char *wifi_ssid = config_get_str("wifi_ssid", NULL);
    bool wifi_connected = false;

    if (wifi_ssid) {
        const char *wifi_pass = config_get_str("wifi_password", "");
        ret = wifi_fetch_init(wifi_ssid, wifi_pass);
        if (ret == ESP_OK) {
            wifi_connected = true;

            /* Upload logs BEFORE rolling (applog_start does rolling). */
            const char *server_url = config_get_str("server_url", "");
            const char *api_key = config_get_str("server_api_key", "");
            wifi_fetch_post_logs(server_url, api_key,
                                 SYSTEM_LOG, LOG_OFFSET_PATH);

            /* Start log capture (with rolling). */
            int log_max_kb = config_get_int("log_max_size_kb", 256);
            applog_start(SYSTEM_LOG, log_max_kb);

            /* Push status. */
            int batt_mv = board_battery_voltage_mv();
            wifi_fetch_status_t status = {
                .battery_connected = board_battery_is_connected() && batt_mv > 1000,
                .battery_percent   = board_battery_percent(),
                .battery_mv        = batt_mv,
                .charging          = board_battery_is_charging(),
                .usb_connected     = board_usb_is_connected(),
                .sd_free_kb        = 0,  /* TODO: implement sd_free_kb */
                .firmware_version  = FIRMWARE_VERSION,
            };
            wifi_fetch_post_status(server_url, api_key, &status);

            /* Fetch next image. */
            ret = wifi_fetch_image(server_url, api_key, &img_buf, &img_size);
            wifi_fetch_deinit();

            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Got %zu bytes from server", img_size);
            } else {
                ESP_LOGW(TAG, "Server image fetch failed, trying SD card");
            }
        } else {
            ESP_LOGW(TAG, "WiFi connect failed, trying SD card");
        }
    }

    /* Start log capture if not already started (no-WiFi path). */
    if (!wifi_connected) {
        int log_max_kb = config_get_int("log_max_size_kb", 256);
        applog_start(SYSTEM_LOG, log_max_kb);
    }

    /* SD card fallback if no image from WiFi. */
    if (!img_buf) {
        char img_path[IMAGE_PICKER_PATH_MAX];
        ret = image_picker_pick(IMAGE_DIR, image_exts, img_path);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No images found in " IMAGE_DIR);
            show_error(frame_buf, "No images found");
            goto unmount;
        }
        ret = image_loader_load(img_path, &img_buf, &img_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load: %s", img_path);
            show_error(frame_buf, "Image load error");
            goto unmount;
        }
    }

    /* JPEG decode → scale → dither into frame buffer */
    ESP_LOGI(TAG, "Decoding %zu bytes", img_size);
    /* ... rest unchanged ... */
```

- [ ] **Step 3: Move config_load before WiFi check**

Ensure `config_load(CONFIG_PATH)` is called right after SD mount, before the WiFi block.

- [ ] **Step 4: Build**

```bash
idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add main/main.c
git commit -m "feat: wire WiFi-first image flow with SD card fallback"
```

### Task 9: Integration test

- [ ] **Step 1: Start the server on a machine on the local network**

```bash
cd server && source venv/bin/activate
PHOTOFRAME_API_KEY=testkey python main.py
```

- [ ] **Step 2: Upload a test image via the web UI**

Open `http://<server-ip>:8080`, upload a baseline JPEG.

- [ ] **Step 3: Configure the frame**

Edit `/sdcard/config.txt`:

```
wifi_ssid=YourNetwork
wifi_password=YourPassword
server_url=http://<server-ip>:8080
server_api_key=testkey
```

- [ ] **Step 4: Flash and test**

```bash
python3 flash.py --timeout 120
```

Expected serial output:
- WiFi connected, got IP
- NTP sync → RTC updated
- Log upload (or "no new bytes")
- Status pushed
- Image downloaded from server
- Decode + display as normal

- [ ] **Step 5: Verify server received status**

Open `http://<server-ip>:8080` — frame should appear in the status panel with battery/firmware info.

- [ ] **Step 6: Test fallback — stop the server, reboot the frame**

Frame should fall back to SD card images.

- [ ] **Step 7: Test fallback — remove wifi_ssid from config.txt**

Frame should skip WiFi entirely and use SD card.

- [ ] **Step 8: Commit any fixes from integration testing**

```bash
git add -A
git commit -m "fix: integration test fixes for WiFi photo retrieval"
```

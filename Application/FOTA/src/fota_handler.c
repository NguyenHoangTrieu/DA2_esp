/*
* Advanced OTA Update Handler for ESP32
* This module manages over-the-air firmware updates using HTTPS.
* It includes features such as image validation, event handling,
* and optional OTA resumption using NVS.
*/
#include "fota_handler.h"
#include "config_handler.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "web_config_handler.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <string.h>
#include <strings.h>

#if FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF
/* The interface name value can refer to if_desc in esp_netif_defaults.h */
#if FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF_ETH
static const char *bind_interface_name = NETIF_DESC_ETH;
#elif FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF_STA
static const char *bind_interface_name = NETIF_DESC_STA;
#endif
#endif

static const char *TAG = "advanced_ota";

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static bool ota_task_close = false;

/* ------------------------------------------------------------------ */
/*  Runtime firmware URL (overridable at runtime via set_url)          */
/* ------------------------------------------------------------------ */
static char s_fota_url[FOTA_CONFIG_FIRMWARE_URL_MAX_LEN] = FOTA_CONFIG_FIRMWARE_URL;

void fota_handler_set_url(const char *url) {
    if (!url || !url[0]) return;
    strncpy(s_fota_url, url, sizeof(s_fota_url) - 1);
    s_fota_url[sizeof(s_fota_url) - 1] = '\0';
    ESP_LOGI(TAG, "[OTA] Firmware URL updated: %s", s_fota_url);
    save_fota_wan_url_to_nvs();
}

const char *fota_handler_get_url(void) {
    return s_fota_url;
}

#if FOTA_CONFIG_ENABLE_OTA_RESUMPTION
#define NVS_NAMESPACE_OTA_RESUMPTION  "ota_resumption"
#define NVS_KEY_OTA_WR_LENGTH  "nvs_ota_wr_len"
#define NVS_KEY_SAVED_URL  "nvs_ota_url"

static esp_err_t ota_res_get_written_len_from_nvs(const nvs_handle_t nvs_handle, 
                                                   const char *url, 
                                                   uint32_t *nvs_ota_wr_len)
{
    esp_err_t err;
    char saved_url[OTA_URL_SIZE] = {0};
    size_t url_len = sizeof(saved_url);

    *nvs_ota_wr_len = 0;

    // Retrieve the saved URL from NVS
    err = nvs_get_str(nvs_handle, NVS_KEY_SAVED_URL, saved_url, &url_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "Saved URL is not initialized yet!");
        return err;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading saved URL (%s)", esp_err_to_name(err));
        return err;
    }

    // Compare the current URL with the saved URL
    if (strcmp(url, saved_url) != 0) {
        ESP_LOGD(TAG, "URLs do not match. Restarting OTA from beginning.");
        return ESP_ERR_INVALID_STATE;
    }

    // Fetch the saved write length only if URLs match
    uint16_t saved_wr_len_kb = 0;
    err = nvs_get_u16(nvs_handle, NVS_KEY_OTA_WR_LENGTH, &saved_wr_len_kb);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "The write length is not initialized yet!");
        *nvs_ota_wr_len = 0;
        return err;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading OTA write length (%s)", esp_err_to_name(err));
        return err;
    }

    // Convert the saved value back to bytes
    *nvs_ota_wr_len = saved_wr_len_kb * 1024;

    return ESP_OK;
}

static esp_err_t ota_res_save_cfg_to_nvs(const nvs_handle_t nvs_handle, 
                                          int nvs_ota_wr_len,  
                                          const char *url)
{
    // Convert the write length to kilobytes to optimize NVS space
    uint16_t wr_len_kb = nvs_ota_wr_len / 1024;

    // Save the current OTA write length to NVS
    ESP_RETURN_ON_ERROR(nvs_set_u16(nvs_handle, NVS_KEY_OTA_WR_LENGTH, wr_len_kb), 
                        TAG, "Failed to set OTA write length");

    // Save the URL only if the OTA write length is non-zero
    if (nvs_ota_wr_len) {
        char saved_url[OTA_URL_SIZE] = {0};
        size_t url_len = sizeof(saved_url);

        esp_err_t err = nvs_get_str(nvs_handle, NVS_KEY_SAVED_URL, saved_url, &url_len);
        if (err == ESP_ERR_NVS_NOT_FOUND || strcmp(saved_url, url) != 0) {
            // URL not saved or changed; save it now
            ESP_RETURN_ON_ERROR(nvs_set_str(nvs_handle, NVS_KEY_SAVED_URL, url), 
                                TAG, "Failed to set URL in NVS");
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error reading OTA URL");
            return err;
        }
    }

    ESP_RETURN_ON_ERROR(nvs_commit(nvs_handle), TAG, "Failed to commit NVS");
    ESP_LOGD(TAG, "Saving state in NVS. Total image written: %d KB", wr_len_kb);
    return ESP_OK;
}

static esp_err_t ota_res_cleanup_cfg_from_nvs(nvs_handle_t handle) 
{
    esp_err_t ret;
    
    // Erase all keys in the NVS handle and commit changes
    ESP_GOTO_ON_ERROR(nvs_erase_all(handle), err, TAG, "Error in erasing NVS");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), err, TAG, "Error in committing NVS");
    ret = ESP_OK;
err:
    nvs_close(handle);
    return ret;
}
#endif

/* Event handler for catching HTTPS OTA events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == ESP_HTTPS_OTA_EVENT) {
        switch (event_id) {
            case ESP_HTTPS_OTA_START:
                ESP_LOGI(TAG, "OTA started");
                break;
            case ESP_HTTPS_OTA_CONNECTED:
                ESP_LOGI(TAG, "Connected to server");
                break;
            case ESP_HTTPS_OTA_GET_IMG_DESC:
                ESP_LOGI(TAG, "Reading Image Description");
                break;
            case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
                ESP_LOGI(TAG, "Verifying chip id of new image: %d", 
                         *(esp_chip_id_t *)event_data);
                break;
            case ESP_HTTPS_OTA_VERIFY_CHIP_REVISION:
                ESP_LOGI(TAG, "Verifying chip revision of new image: %d", 
                         *(esp_chip_id_t *)event_data);
                break;
            case ESP_HTTPS_OTA_DECRYPT_CB:
                ESP_LOGI(TAG, "Callback to decrypt function");
                break;
            case ESP_HTTPS_OTA_WRITE_FLASH:
                ESP_LOGD(TAG, "Writing to flash: %d written", *(int *)event_data);
                break;
            case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
                ESP_LOGI(TAG, "Boot partition updated. Next Partition: %d", 
                         *(esp_partition_subtype_t *)event_data);
                break;
            case ESP_HTTPS_OTA_FINISH:
                ESP_LOGI(TAG, "OTA finish");
                break;
            case ESP_HTTPS_OTA_ABORT:
                ESP_LOGI(TAG, "OTA abort");
                break;
        }
    }
}

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }

#if FOTA_CONFIG_SKIP_VERSION_CHECK
    if (memcmp(new_app_info->version, running_app_info.version, 
               sizeof(new_app_info->version)) == 0) {
        ESP_LOGW(TAG, "Current running version is the same as new. Update cancelled.");
        return ESP_FAIL;
    }
#endif

#if FOTA_CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    const uint32_t hw_sec_version = esp_efuse_read_secure_version();
    if (new_app_info->secure_version < hw_sec_version) {
        ESP_LOGW(TAG, "New firmware security version is less than eFuse programmed, %d < %d", 
                 new_app_info->secure_version, hw_sec_version);
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
}

static esp_err_t _http_client_init_cb(esp_http_client_handle_t http_client)
{
    esp_err_t err = ESP_OK;
    return err;
}

static void print_sha256(const uint8_t *image_hash, const char *label) 
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void) 
{
    uint8_t sha_256[HASH_LEN] = {0};
    esp_partition_t partition;

    // get sha256 digest for bootloader
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size = ESP_PARTITION_TABLE_OFFSET;
    partition.type = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader:");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware:");
}

/* ------------------------------------------------------------------ */
/*  Connectivity pre-check: TCP connect to host:port parsed from URL   */
/* ------------------------------------------------------------------ */
static bool server_reachable(void) {
    /* Parse host and port from FOTA_CONFIG_FIRMWARE_URL at runtime so
     * any URL format works (ThingsBoard, GitHub, custom HTTP, etc.). */
    const char *url = fota_handler_get_url();
    const char *after_scheme = strstr(url, "://");
    if (!after_scheme) {
        ESP_LOGW(TAG, "[OTA-CHECK] Malformed URL: %s", url);
        return false;
    }
    after_scheme += 3; /* skip "://" */

    /* Extract host (up to ':' or '/' or end) */
    char host[128] = {0};
    const char *port_ptr = NULL;
    const char *slash    = strchr(after_scheme, '/');
    const char *colon    = strchr(after_scheme, ':');
    /* colon must be before the first slash to be a port separator */
    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - after_scheme);
        if (hlen >= (int)sizeof(host)) hlen = (int)sizeof(host) - 1;
        memcpy(host, after_scheme, hlen);
        port_ptr = colon + 1;
    } else {
        int hlen = slash ? (int)(slash - after_scheme) : (int)strlen(after_scheme);
        if (hlen >= (int)sizeof(host)) hlen = (int)sizeof(host) - 1;
        memcpy(host, after_scheme, hlen);
    }

    char port_str[8] = "80";
    if (port_ptr) {
        int plen = slash ? (int)(slash - port_ptr) : (int)strlen(port_ptr);
        if (plen > 0 && plen < (int)sizeof(port_str))
            memcpy(port_str, port_ptr, plen);
    } else if (strncmp(url, "https", 5) == 0) {
        strcpy(port_str, "443");
    }

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "[OTA-CHECK] DNS failed for %s", host);
        return false;
    }

    int sock = socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return false; }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int timeout_ms = FOTA_CONFIG_CONNECTIVITY_CHECK_TIMEOUT_MS;
    uint32_t t0 = esp_log_timestamp();
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    if (r < 0 && errno == EINPROGRESS) {
        struct timeval tv = {.tv_sec  = timeout_ms / 1000,
                             .tv_usec = (timeout_ms % 1000) * 1000};
        fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel > 0) {
            int so_err = 0; socklen_t sl = sizeof(so_err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &sl);
            r = (so_err == 0) ? 0 : -1;
        } else { r = -1; }
    }
    uint32_t ms = esp_log_timestamp() - t0;
    close(sock);
    freeaddrinfo(res);

    if (r == 0) {
        ESP_LOGI(TAG, "[OTA-CHECK] %s:%s reachable (%lums)", host, port_str, (unsigned long)ms);
        return true;
    }
    ESP_LOGW(TAG, "[OTA-CHECK] %s:%s unreachable (%lums) errno=%d",
             host, port_str, (unsigned long)ms, errno);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Event handler to capture redirect Location response header          */
/* ------------------------------------------------------------------ */
static char s_wan_location_header[2048];
static esp_err_t wan_ota_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            strlcpy(s_wan_location_header, evt->header_value,
                    sizeof(s_wan_location_header));
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  OTA download via esp_http_client                                    */
/* ------------------------------------------------------------------ */
static esp_err_t ota_download(void) {
    const char *fw_url = fota_handler_get_url();
    ESP_LOGI(TAG, "[OTA] Downloading from: %s", fw_url);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "[OTA] No next OTA partition");
        return ESP_FAIL;
    }

    char *buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(4096);
    if (!buf) return ESP_ERR_NO_MEM;

    /* Resolve redirects first via lightweight probe requests. The
     * HTTP_EVENT_ON_HEADER callback captures the Location response header
     * (esp_http_client_get_header() only reads request headers — wrong API). */
    static char s_final_url[2048];
    strlcpy(s_final_url, fw_url, sizeof(s_final_url));

    for (int redir = 0; redir < 5; redir++) {
        s_wan_location_header[0] = '\0';
        bool cur_https = (strncmp(s_final_url, "https://", 8) == 0);
        esp_http_client_config_t probe_cfg = {
            .url               = s_final_url,
            .timeout_ms        = 10000,
            .buffer_size       = 2048,
            .buffer_size_tx    = 512,
            .keep_alive_enable = false,
            .event_handler     = wan_ota_http_event_handler,
            .crt_bundle_attach = cur_https ? esp_crt_bundle_attach : NULL,
        };
        esp_http_client_handle_t probe = esp_http_client_init(&probe_cfg);
        if (!probe) { ESP_LOGE(TAG, "[OTA] probe init failed"); free(buf); return ESP_FAIL; }
        esp_http_client_open(probe, 0);
        esp_http_client_fetch_headers(probe);
        int pstatus = esp_http_client_get_status_code(probe);
        esp_http_client_cleanup(probe);
        if (pstatus == 301 || pstatus == 302 || pstatus == 307 || pstatus == 308) {
            if (s_wan_location_header[0] == '\0') {
                ESP_LOGE(TAG, "[OTA] Redirect %d: no Location header", redir + 1);
                free(buf); return ESP_FAIL;
            }
            ESP_LOGI(TAG, "[OTA] Redirect %d (%d) → %s", redir + 1, pstatus, s_wan_location_header);
            strlcpy(s_final_url, s_wan_location_header, sizeof(s_final_url));
        } else {
            break;
        }
    }

    ESP_LOGI(TAG, "[OTA] Final URL: %s", s_final_url);
    bool use_https = (strncmp(s_final_url, "https://", 8) == 0);
    esp_http_client_config_t http_cfg = {
        .url               = s_final_url,
        .timeout_ms        = FOTA_CONFIG_OTA_RECV_TIMEOUT,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
        .keep_alive_enable = false,
        .crt_bundle_attach = use_https ? esp_crt_bundle_attach : NULL,
    };

    esp_err_t ret = ESP_FAIL;
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "[OTA] esp_http_client_init failed");
        free(buf);
        return ESP_FAIL;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] HTTP open failed");
        goto cleanup_client;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[OTA] HTTP %d, Content-Length: %lld", http_status, content_len);

    if (http_status != 200) {
        ESP_LOGE(TAG, "[OTA] HTTP error %d (expected 200)", http_status);
        goto cleanup_client;
    }

    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    if (esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] esp_ota_begin failed");
        goto cleanup_client;
    }
    ota_started = true;

    int total = 0;
    uint32_t t0 = esp_log_timestamp();
    while (true) {
        int len = esp_http_client_read(client, buf, 4096);
        if (len == 0) break;
        if (len < 0) {
            ESP_LOGE(TAG, "[OTA] Read error %d at %d bytes", len, total);
            goto cleanup_ota;
        }
        if (esp_ota_write(ota_handle, buf, len) != ESP_OK) {
            ESP_LOGE(TAG, "[OTA] esp_ota_write failed");
            goto cleanup_ota;
        }
        total += len;
        if (total % (64 * 1024) < 4096)
            ESP_LOGI(TAG, "[OTA] Progress: %d bytes", total);
    }

    {
        uint32_t ms = esp_log_timestamp() - t0;
        ESP_LOGI(TAG, "[OTA] %d bytes in %lums (%ld B/s)", total,
                 (unsigned long)ms, ms > 0 ? (long)(total * 1000L / ms) : 0);
    }

    if (total == 0) {
        ESP_LOGE(TAG, "[OTA] Zero bytes received");
        goto cleanup_ota;
    }

    ret = esp_ota_end(ota_handle);
    ota_started = false;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] esp_ota_end: %s", esp_err_to_name(ret));
        goto cleanup_client;
    }
    ret = esp_ota_set_boot_partition(update);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] set_boot_partition: %s", esp_err_to_name(ret));
    }
    goto cleanup_client;

cleanup_ota:
    if (ota_started) esp_ota_abort(ota_handle);
cleanup_client:
    free(buf);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ret;
}

void advanced_ota_task(void *pvParameter) 
{
    ESP_LOGI(TAG, "Starting Advanced OTA - V2.0.0");
    ESP_LOGI(TAG, "[OTA] Target: %s", fota_handler_get_url());

    get_sha256_of_partitions();

    const int max_retries = 5;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        uint32_t t0 = esp_log_timestamp();
        ESP_LOGI(TAG, "OTA attempt %d/%d", attempt, max_retries);

        if (!server_reachable()) {
            ESP_LOGW(TAG, "[OTA] ThingsBoard not reachable, waiting 15s...");
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        err = ota_download();

        uint32_t elapsed = esp_log_timestamp() - t0;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA succeeded after %lums, rebooting...", (unsigned long)elapsed);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        ESP_LOGE(TAG, "OTA attempt %d failed after %lums: %s",
                 attempt, (unsigned long)elapsed, esp_err_to_name(err));

        if (attempt < max_retries)
            vTaskDelay(pdMS_TO_TICKS(15000));
    }

    ESP_LOGE(TAG, "OTA failed after %d attempts, rebooting", max_retries);
    esp_restart();
    vTaskDelete(NULL);
}

void fota_handler_task_start(void)
{
    ota_task_close = false;

    /* Stop web config portal to free heap for OTA download buffer */
    ESP_LOGI(TAG, "Stopping web config portal to free heap for OTA");
    web_config_handler_stop();

    size_t internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Heap before OTA task: total=%d, internal=%d, internal_largest=%d",
             esp_get_free_heap_size(), internal_free, internal_largest);

    /* IMPORTANT: stack MUST be in internal RAM for mbedTLS crypto performance.
     * 12KB is sufficient: mbedTLS handshake depth ~3KB, OTA locals ~1KB.
     * If internal RAM fragmented, fallback to PSRAM (slower TLS but still works). */
    BaseType_t ret = xTaskCreate(&advanced_ota_task, "advanced_ota_task", 12 * 1024, NULL, 5, NULL);
    if (ret != pdPASS) {
        /* Internal RAM stack failed — try PSRAM (if available).
         * PSRAM stack is slower for crypto (L1$ misses) but better than no task. */
        ESP_LOGW(TAG, "Internal RAM stack failed (largest=%d), retrying in PSRAM", internal_largest);
        ret = xTaskCreateWithCaps(&advanced_ota_task, "advanced_ota_task",
                                  16 * 1024, NULL, 5, NULL,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create Advanced OTA task (internal AND PSRAM)");
            return;
        }
    }
    ESP_LOGI(TAG, "Advanced OTA task created successfully");
}

void fota_handler_task_stop(void) 
{ 
    ota_task_close = true; 
}
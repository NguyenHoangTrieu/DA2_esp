/*
 * fota_ap.c — FOTA SoftAP for LAN MCU firmware updates
 *
 * Creates a WiFi access point on the WAN MCU so the LAN MCU can connect
 * as a station and download firmware from ThingsBoard directly over WiFi.
 * NAPT routes packets from the AP subnet to whatever internet interface
 * the WAN MCU is currently using (WiFi STA, LTE, or Ethernet).
 *
 * This eliminates PPP/UART as an internet path for FOTA.  WiFi is far
 * more reliable: no UART flow-control issues, no FIFO overflows, no
 * priority inversion with the LwIP task.
 */

#include "fota_ap.h"
#include "config_handler.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "fota_ap";

/* State ------------------------------------------------------------------ */
static esp_netif_t *s_fota_ap_netif  = NULL;
static bool         s_fota_ap_running = false;
/* Track whether WiFi was already running when we started (APSTA vs AP). */
static bool         s_was_apsta       = false;

/* Called by the event-loop task when WIFI_EVENT_AP_START is dispatched.
 * Arg is a SemaphoreHandle_t created locally in fota_ap_start().
 * Registered AFTER esp_netif_create_default_wifi_ap() — that call registers
 * esp_netif's own WIFI_EVENT_AP_START handler which sets the lwIP netif UP.
 * Because handlers run in registration order, ours fires AFTER esp_netif's,
 * guaranteeing the lwIP netif is UP by the time the semaphore is given. */
static void fota_ap_started_cb(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)base; (void)event_id; (void)event_data;
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

/* ========================================================================
 *  Public API
 * ======================================================================== */

esp_err_t fota_ap_start(void)
{
    if (s_fota_ap_running) {
        ESP_LOGI(TAG, "FOTA AP already running — no-op");
        return ESP_OK;
    }

    /* ------------------------------------------------------------------
     * 1. Create the AP netif (must happen before esp_wifi_set_mode).
     * ------------------------------------------------------------------ */
    if (!s_fota_ap_netif) {
        s_fota_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_fota_ap_netif) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap() failed");
            return ESP_FAIL;
        }
    }

    /* ------------------------------------------------------------------
     * 2. Determine current WiFi state.
     * ------------------------------------------------------------------ */
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    bool wifi_already_up = (esp_wifi_get_mode(&current_mode) == ESP_OK &&
                            current_mode != WIFI_MODE_NULL);

    /* ------------------------------------------------------------------
     * 3. Set AP config before changing the mode.
     * ------------------------------------------------------------------ */
    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = FOTA_AP_CHANNEL,
            .max_connection = 2,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid,     FOTA_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, FOTA_AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(FOTA_AP_SSID);

    /* ------------------------------------------------------------------
     * 4. Register a one-shot WIFI_EVENT_AP_START handler BEFORE
     *    changing the WiFi mode.  We must block until esp_netif's
     *    own handler has run — that handler calls netif_set_up() on
     *    the lwIP netif.  esp_netif_napt_enable() → ip_napt_enable_netif()
     *    checks netif_is_up() and returns ERR_VAL if not up → ESP_FAIL.
     *
     *    esp_netif_create_default_wifi_ap() (step 1 above) registered
     *    esp_netif's handler first; registering ours now means ours fires
     *    AFTER esp_netif's (FIFO dispatch order).  By the time our
     *    semaphore is given, the lwIP netif is guaranteed to be UP.
     *
     *    Only needed when the AP is being freshly started (not when it
     *    is already running in AP/APSTA mode).
     * ------------------------------------------------------------------ */
    bool wait_for_ap_start = (wifi_already_up && current_mode == WIFI_MODE_STA)
                             || !wifi_already_up;
    SemaphoreHandle_t ap_sem = wait_for_ap_start ? xSemaphoreCreateBinary() : NULL;
    if (ap_sem) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                    fota_ap_started_cb, ap_sem);
    }

    /* ------------------------------------------------------------------
     * 5. Enable WiFi in the right mode.
     * ------------------------------------------------------------------ */
    if (wifi_already_up && current_mode == WIFI_MODE_STA) {
        /* WiFi STA is connected — add AP alongside it (APSTA).
         * The STA association is preserved; only the mode register changes. */
        ESP_LOGI(TAG, "WiFi STA active → switching to APSTA mode");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        s_was_apsta = true;
    } else if (!wifi_already_up) {
        /* WiFi was not running (LTE / Ethernet case) — bring it up as AP. */
        ESP_LOGI(TAG, "WiFi idle (LTE/Ethernet) → starting WiFi in AP mode");
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t init_ret = esp_wifi_init(&cfg);
        if (init_ret != ESP_OK && init_ret != ESP_ERR_WIFI_INIT_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(init_ret));
            if (ap_sem) {
                esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START,
                                              fota_ap_started_cb);
                vSemaphoreDelete(ap_sem);
            }
            return init_ret;
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        s_was_apsta = false;
    } else {
        /* Already in AP or APSTA — just reconfigure. */
        ESP_LOGI(TAG, "WiFi mode=%d — reconfiguring AP", current_mode);
        s_was_apsta = (current_mode == WIFI_MODE_APSTA);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (!wifi_already_up) {
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    /* ------------------------------------------------------------------
     * 6. Block until WIFI_EVENT_AP_START has been fully processed.
     *    Our handler (registered after esp_netif's) is the last to run,
     *    so by the time xSemaphoreTake returns the lwIP netif is UP.
     * ------------------------------------------------------------------ */
    if (ap_sem) {
        if (xSemaphoreTake(ap_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "Timed out waiting for WIFI_EVENT_AP_START (2s)");
        } else {
            ESP_LOGI(TAG, "WIFI_EVENT_AP_START processed — lwIP netif is UP");
        }
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START,
                                      fota_ap_started_cb);
        vSemaphoreDelete(ap_sem);
    }

    /* ------------------------------------------------------------------
     * 7. Enable NAPT on the AP netif so the LAN MCU's traffic is
     *    forwarded to the internet interface automatically.
     *    The lwIP netif is now guaranteed UP — napt_enable will succeed.
     * ------------------------------------------------------------------ */
    esp_err_t napt_ret = esp_netif_napt_enable(s_fota_ap_netif);
    if (napt_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(napt_ret));
        /* Non-fatal — AP still usable, but no internet routing */
    }

    s_fota_ap_running = true;
    ESP_LOGI(TAG,
             "FOTA AP started — SSID=\"" FOTA_AP_SSID
             "\"  ch=%d  NAPT=%s",
             FOTA_AP_CHANNEL,
             (napt_ret == ESP_OK) ? "OK" : "FAIL");
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */

esp_err_t fota_ap_stop(void)
{
    if (!s_fota_ap_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping FOTA AP");

    if (s_was_apsta) {
        /* Revert to STA-only mode; the STA association is still alive. */
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else {
        /* WiFi was started only for the AP — shut it down completely. */
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    if (s_fota_ap_netif) {
        esp_netif_destroy(s_fota_ap_netif);
        s_fota_ap_netif = NULL;
    }

    s_fota_ap_running = false;
    s_was_apsta       = false;
    ESP_LOGI(TAG, "FOTA AP stopped");
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */

bool fota_ap_is_running(void)
{
    return s_fota_ap_running;
}

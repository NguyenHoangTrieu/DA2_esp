/**
 * @file lte_connect.c
 * @brief LTE connection manager with config handler integration
 */

#include "lte_connect.h"
#include "config_handler.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lte_handler.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "pcf8563_rtc.h"
#include "stack_handler.h"
#include "usbh_modem_board.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ==================== Default Configuration ==================== */
/* Default APN for Vietnamobile (m-wap).
 * The LTE task will start with this APN unless config_handler overrides it. */
#define LTE_DEFAULT_APN "m-wap"
#define LTE_CONNECTION_MONITOR_INTERVAL_MS 1000

static const char *TAG = "LTE_CONNECT";
static bool g_lte_sntp_synced = false;
static bool g_lte_sntp_started = false;

/* ==================== LTE Config Context ==================== */
lte_config_context_t g_lte_ctx = {
    .modem_name = "",
    .apn = LTE_DEFAULT_APN, /* Default Vietnamobile m-wap */
    .username = "",
    .password = "",
    .max_reconnect_attempts = 0, /* 0 = infinite once configured      */
    .reconnect_timeout_ms = 30000,
    .auto_reconnect = false, /* disabled until config received     */
    .comm_type = LTE_HANDLER_USB,
    .pwr_pin = 5, /* P05 (flat GPIO mapping, numeric ID) */
    .rst_pin = 6, /* P06 (flat GPIO mapping, numeric ID) */
    .initialized = false,
    .task_running = false,
    .task_handle = NULL,
};

/**
 * @brief Initialize/Reinitialize LTE with current config
 */
static esp_err_t lte_init_with_config(void) {
  if (g_lte_ctx.initialized) {
    ESP_LOGI(TAG, "Reinitializing LTE...");
    lte_handler_disconnect();
    lte_handler_deinit();
    vTaskDelay(pdMS_TO_TICKS(1000));
    g_lte_ctx.initialized = false;
  }

  lte_handler_config_t cfg = {
      .comm_type = g_lte_ctx.comm_type,
      .apn = g_lte_ctx.apn,
      .username = g_lte_ctx.username[0] ? g_lte_ctx.username : NULL,
      .password = g_lte_ctx.password[0] ? g_lte_ctx.password : NULL,
      .auto_reconnect = g_lte_ctx.auto_reconnect,
      .reconnect_timeout_ms = g_lte_ctx.reconnect_timeout_ms,
      .max_reconnect_attempts = g_lte_ctx.max_reconnect_attempts};

  ESP_LOGI(TAG, "Initializing LTE - Modem: %s, APN: %s, Type: %s",
           g_lte_ctx.modem_name, cfg.apn,
           cfg.comm_type == LTE_HANDLER_UART ? "UART" : "USB");

  /* Configure TCA GPIO pins and modem name before init */
  modem_board_set_tca_pins(g_lte_ctx.pwr_pin, g_lte_ctx.rst_pin);
  modem_board_set_modem_name(g_lte_ctx.modem_name);

  esp_err_t ret = lte_handler_init(&cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init: 0x%x", ret);
    return ret;
  }

  g_lte_ctx.initialized = true;

  ret = lte_handler_connect();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
    /* Don't fail - auto-reconnect will handle */
  }

  return ret;
}

static void lte_sntp_sync_notification_cb(struct timeval *tv) {
  ESP_LOGI(TAG, "LTE SNTP time synchronized!");
  g_lte_sntp_synced = true;

  /* Slow down SNTP polling after first sync — 30 s was only for initial
   * retry speed; continuing at 30 s wastes data and floods the log.
   * 1 hour is more than sufficient for ongoing time maintenance.        */
  sntp_set_sync_interval(3600 * 1000);

  // Sync system time to PCF8563
  time_t now = tv->tv_sec;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  esp_err_t ret = pcf8563_write_time(&timeinfo);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "System time synced to PCF8563 RTC");
  } else {
    ESP_LOGW(TAG, "Failed to sync time to PCF8563: %s", esp_err_to_name(ret));
  }
}

static void lte_init_sntp_once(void) {
  if (g_lte_sntp_started)
    return;

  ESP_LOGI(TAG, "Initializing SNTP (LTE)");

  /* TZ VN */
  setenv("TZ", "ICT-7", 1);
  tzset();

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  /* Use hardcoded IP addresses so SNTP never needs DNS resolution.
   * If the carrier DNS is slow/unreliable, hostname-based servers put
   * SNTP into its exponential retry loop (15 s → 30 s → …).          */
  esp_sntp_setservername(0, "216.239.35.0");  /* time1.google.com */
  esp_sntp_setservername(1, "162.159.200.1"); /* time.cloudflare.com */
  esp_sntp_setservername(2, "pool.ntp.org");  /* hostname fallback */

  esp_sntp_set_time_sync_notification_cb(lte_sntp_sync_notification_cb);
  /* Set timeout so SNTP doesn't block indefinitely. After 30 s of retries,
   * SNTP will give up and periodic retries will continue in background.
   * This prevents the boot from hanging if NTP is unreachable.           */
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  sntp_set_sync_interval(30 * 1000); /* Start periodic retry after 30 s */

  esp_sntp_init();
  g_lte_sntp_started = true;

  ESP_LOGI(TAG, "SNTP initialized, waiting for sync...");
}

bool lte_is_sntp_synced(void) { return g_lte_sntp_synced; }

/**
 * @brief Combined task: monitor connection + handle config updates
 */
static void lte_task(void *arg) {
  ESP_LOGI(TAG, "LTE task started");

  TickType_t last_monitor = 0;
  TickType_t last_reconnect_attempt = 0;
  const TickType_t monitor_interval =
      pdMS_TO_TICKS(LTE_CONNECTION_MONITOR_INTERVAL_MS);
  const TickType_t reconnect_interval = pdMS_TO_TICKS(10000); // Retry every 10s

  uint32_t reconnect_count = 0;

  /* Wait for modem power-on before initializing (module needs ~15s after VCC)
   */
  extern config_internet_type_t g_internet_type;
  if (g_internet_type == CONFIG_INTERNET_LTE) {
    ESP_LOGI(TAG, "LTE: waiting 15 s for modem power-on...");
    for (int i = 15; i > 0 && g_lte_ctx.task_running; i--) {
      ESP_LOGD(TAG, "LTE modem startup: %d s remaining...", i);
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } else {
    ESP_LOGI(TAG, "LTE: Fallback mode - skipping 15s power-on wait");
  }

  /* Initial connection */
  lte_init_with_config();

  while (g_lte_ctx.task_running) {
    /* Check for config updates from config_handler */
    lte_config_data_t new_cfg;
    if (g_lte_config_queue && xQueueReceive(g_lte_config_queue, &new_cfg,
                                            pdMS_TO_TICKS(100)) == pdTRUE) {

      ESP_LOGI(TAG, "Received new LTE config");
      ESP_LOGI(TAG, "APN: %s, Username: %s", new_cfg.apn, new_cfg.username);

      /* Update internal config */
      strncpy(g_lte_ctx.apn, new_cfg.apn, sizeof(g_lte_ctx.apn) - 1);
      strncpy(g_lte_ctx.username, new_cfg.username,
              sizeof(g_lte_ctx.username) - 1);
      strncpy(g_lte_ctx.password, new_cfg.password,
              sizeof(g_lte_ctx.password) - 1);
      g_lte_ctx.comm_type = new_cfg.comm_type;
      g_lte_ctx.auto_reconnect = new_cfg.auto_reconnect;
      g_lte_ctx.reconnect_timeout_ms = new_cfg.reconnect_timeout_ms;
      g_lte_ctx.max_reconnect_attempts = new_cfg.max_reconnect_attempts;

      /* Reinitialize with new config */
      reconnect_count = 0; // Reset counter
      lte_init_with_config();
    }

    /* Periodic monitoring */
    TickType_t now = xTaskGetTickCount();
    if ((now - last_monitor) >= monitor_interval) {
      if (g_lte_ctx.initialized) {
        if (lte_handler_is_connected()) {
          if (!is_internet_connected) {
            is_internet_connected = true;
            /* Initialize application status to ONLINE now that PPP is
               successfully UP. This prevents the Zombie Link Detector from
               triggering prematurely on boot */
            mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
          }

          lte_init_sntp_once();
          reconnect_count = 0; // Reset on successful connection

          static bool s_dns_applied = false;
          if (!s_dns_applied) {
            /* Apply public DNS ONCE per connection.
             * LwIP's dns.c aborts concurrent queries and causes EAI_FAIL (202)
             * if we continuously spam dns_setserver() every 1000ms. */
            ip_addr_t dns_a;
            IP4_ADDR(&dns_a.u_addr.ip4, 8, 8, 8, 8);
            dns_a.type = IPADDR_TYPE_V4;
            dns_setserver(0, &dns_a);
            IP4_ADDR(&dns_a.u_addr.ip4, 1, 1, 1, 1);
            dns_setserver(1, &dns_a);
            s_dns_applied = true;
          }

          /* Zombie IP Context Detector:
           * Even if the PPP interface says UP, the cellular network often
           * silently drops the TCP/UDP routing behind NAT after idling for
           * 30-60s. The upper handlers (MQTT, HTTP, CoAP) will experience
           * timeouts / failures and set internet_status to OFFLINE. If the
           * application consistently reports OFFLINE for 180 seconds despite
           * PPP being UP, the cellular context is dead. (Threshold is > MQTT
           * reconnect delay to avoid false positives). */
          static uint32_t offline_timeout_ticks = 0;
          if (mcu_lan_handler_get_internet_status() ==
              INTERNET_STATUS_OFFLINE) {
            offline_timeout_ticks++;
            if (offline_timeout_ticks >= 180) {
              ESP_LOGE(TAG, "Application reported OFFLINE for 180s. Zombie "
                            "link detected, restarting LTE...");
              lte_init_with_config(); // Full hardware de-init and power cycle
                                      // to fix frozen modems
              offline_timeout_ticks = 0;
              s_dns_applied = false; // Reset for next connect
            }
          } else {
            offline_timeout_ticks = 0;
          }
        } else {
          /* PPP link is down — immediately clear the internet flag so that
           * MQTT/CoAP/HTTP built-in auto-reconnect backs off instead of
           * hammering TCP connects while modem_board is re-negotiating PPP.
           * The flag is set back to true above as soon as PPP is UP again. */
          is_internet_connected = false;

          ESP_LOGW(TAG, "LTE not connected - State: %d",
                   lte_handler_get_state());

          /* Always attempt active reconnect (auto_reconnect flag only
           * gates whether the middleware-level bg_task reconnects; we
           * always want the application-level task to recover PPP).   */
          if (g_lte_ctx.max_reconnect_attempts == 0 ||
              reconnect_count < g_lte_ctx.max_reconnect_attempts) {

            /* Throttle: don't call lte_handler_connect() faster than every
             * 10 s — modem_board's own STAGE_WAIT_IP loop needs time.   */
            if ((now - last_reconnect_attempt) >= reconnect_interval) {
              ESP_LOGI(TAG, "LTE reconnect attempt %lu...",
                       reconnect_count + 1);

              esp_err_t ret = lte_handler_connect();
              if (ret == ESP_OK) {
                ESP_LOGI(TAG, "LTE reconnect initiated");
              } else {
                ESP_LOGW(TAG, "LTE reconnect call failed: 0x%x", ret);
              }
              reconnect_count++;
              last_reconnect_attempt = now;
            }
          } else {
            ESP_LOGE(TAG, "LTE max reconnect attempts reached (%lu)",
                     reconnect_count);
          }
        }
      }
      last_monitor = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }

  ESP_LOGI(TAG, "LTE task stopped");
  g_lte_ctx.task_handle = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief Start LTE connection
 */
void lte_connect_task_start(void) {
  if (g_lte_ctx.task_running) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  /* Log the APN being used (default or configured) */
  if (g_lte_ctx.apn[0] == '\0') {
    ESP_LOGE(TAG, "LTE task not started: APN is empty. "
                  "Send \"LT:MODEM_NAME:APN:...\" command to configure.");
    return;
  }

  ESP_LOGI(TAG, "Starting LTE connect with APN: %s", g_lte_ctx.apn);

  g_lte_ctx.task_running = true;
  g_lte_ctx.initialized = false;

  BaseType_t ret =
      xTaskCreate(lte_task, "lte_task", 16384, NULL, 5, &g_lte_ctx.task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    g_lte_ctx.task_running = false;
    return;
  }

  ESP_LOGI(TAG, "LTE connect started");
}

/**
 * @brief Stop LTE connection
 */
void lte_connect_task_stop(void) {
  if (!g_lte_ctx.task_running) {
    ESP_LOGW(TAG, "Not running");
    return;
  }

  ESP_LOGI(TAG, "Stopping LTE connect...");

  g_lte_ctx.task_running = false;

  /* Wait for task to exit */
  if (g_lte_ctx.task_handle) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    g_lte_ctx.task_handle = NULL;
  }

  /* Cleanup */
  if (g_lte_ctx.initialized) {
    if (lte_handler_is_connected()) {
      lte_handler_disconnect();
      // Wait a bit for clean disconnect before deinit
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
    lte_handler_deinit();
    g_lte_ctx.initialized = false;
  }

  ESP_LOGI(TAG, "LTE connect stopped");
}

/*
 * ESP32-S3 USB Host Flash Bridge (Rewritten)
 * Receives firmware from PC via UART and flashes target ESP32 WROOM via USB
 * Host Uses native USB Host Library without CDC-ACM component dependency
 */

#include "DA2_esp.h"

static const char *TAG = "MAIN APP";

TaskHandle_t main_task_handle = NULL;

/**
 * @brief APP event group
 *
 * APP_EVENT            - General event, which is APP_QUIT_PIN press event in
 * this example.
 */
typedef enum {
  APP_EVENT_PUSH = 0,
} app_event_group_t;

/**
 * @brief APP event queue
 *
 * This event is used for delivering events from callback to a task.
 */
typedef struct {
  app_event_group_t event_group;
} app_event_queue_t;

static uint32_t last_isr_tick = 0;

static void gpio45_isr_handler(void *arg) {
  uint32_t now = xTaskGetTickCountFromISR();
  if ((now - last_isr_tick) >= pdMS_TO_TICKS(500)) {
    last_isr_tick = now;
    BaseType_t xTaskWoken = pdFALSE;
    if (main_task_handle) {
      vTaskNotifyGiveFromISR(main_task_handle, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

void setup_gpio45_interrupt(void) {
  gpio_config_t gpio45_cfg = {.pin_bit_mask = BIT64(45),
                              .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE,
                              .intr_type = GPIO_INTR_NEGEDGE};
  // Configure GPIO45 and install ISR
  ESP_ERROR_CHECK(gpio_config(&gpio45_cfg));
  // Install GPIO ISR service
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
  // Attach the interrupt handler for GPIO45
  ESP_ERROR_CHECK(gpio_isr_handler_add(45, gpio45_isr_handler, NULL));
}

// =============================================================================
// Main Application Entry Point
// =============================================================================

/**
 * @brief Main application entry point
 */
void app_main(void) {
  ESP_LOGI(TAG, "USB host library example");
  init_led_strip();
  led_on();
  setup_gpio45_interrupt();
  main_task_handle = xTaskGetCurrentTaskHandle();
  uint8_t change = 0;

  // NVS Initialize
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Start USB tasks
  usb_host_lib_task_start();
  class_driver_task_start();
  usb_otg_rw_task_start();

  while (1) {
    ulTaskNotifyTake(pdTRUE,
                     portMAX_DELAY); // Wait for notify from ISR (button press)
    if (APP_EVENT_PUSH == APP_EVENT_PUSH) {
      // User pressed button
      if (change == 0) {
        change = 1;
        ESP_LOGI(TAG, "Button pressed, switch to jtag");
        usb_otg_rw_task_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        class_driver_task_stop();
        usb_host_lib_task_stop();
        jtag_task_start();
        led_show_yellow();
      } else {
        change = 0;
        ESP_LOGI(TAG, "Button pressed, switch to USB Host");
        jtag_task_stop();
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for jtag task to close
        usb_host_lib_task_start();
        class_driver_task_start();
        usb_otg_rw_task_start();
        led_show_orange();
      }
    }
  }
}

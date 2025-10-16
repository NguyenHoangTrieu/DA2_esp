/*
 * ESP32-S3 USB Host Flash Bridge (Rewritten)
 * Receives firmware from PC via UART and flashes target ESP32 WROOM via USB Host
 * Uses native USB Host Library without CDC-ACM component dependency
 */

#include "usb_flash_handler.h"
#include "rbg_handler.h"

static const char *TAG = "MAIN APP";

QueueHandle_t app_event_queue = NULL;

/**
 * @brief APP event group
 *
 * APP_EVENT            - General event, which is APP_QUIT_PIN press event in this example.
 */
typedef enum {
    APP_EVENT = 0,
} app_event_group_t;

/**
 * @brief APP event queue
 *
 * This event is used for delivering events from callback to a task.
 */
typedef struct {
    app_event_group_t event_group;
} app_event_queue_t;

/**
 * @brief BOOT button pressed callback
 *
 * Signal application to exit the Host lib task
 *
 * @param[in] arg Unused
 */
static void gpio_cb(void *arg)
{
    const app_event_queue_t evt_queue = {
        .event_group = APP_EVENT,
    };

    BaseType_t xTaskWoken = pdFALSE;

    if (app_event_queue) {
        xQueueSendFromISR(app_event_queue, &evt_queue, &xTaskWoken);
    }

    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// =============================================================================
// Main Application Entry Point
// =============================================================================

/**
 * @brief Main application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "USB host library example");

    // Init BOOT button: Pressing the button simulates app request to exit
    // It will uninstall the class driver and USB Host Lib
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));
    init_led_strip();
    led_on();
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
    app_event_queue_t evt_queue;

    TaskHandle_t host_lib_task_hdl, class_driver_task_hdl;

    // Create usb host lib task
    BaseType_t task_created;
    task_created = xTaskCreatePinnedToCore(usb_host_lib_task,
                                           "usb_host",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           HOST_LIB_TASK_PRIORITY,
                                           &host_lib_task_hdl,
                                           0);
    assert(task_created == pdTRUE);

    // Wait until the USB host library is installed
    ulTaskNotifyTake(false, 1000);

    // Create class driver task
    task_created = xTaskCreatePinnedToCore(class_driver_task,
                                           "class",
                                           5 * 1024,
                                           NULL,
                                           CLASS_TASK_PRIORITY,
                                           &class_driver_task_hdl,
                                           0);
    assert(task_created == pdTRUE);

    task_created = xTaskCreatePinnedToCore(usb_otg_rw_task,
                                           "usb_otg_rw",
                                           4 * 1024,
                                           NULL,
                                           CLASS_TASK_PRIORITY,
                                           NULL,
                                           0);
    assert(task_created == pdTRUE);
    // Add a short delay to let the tasks run
    vTaskDelay(10);

    while (1) {
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT == evt_queue.event_group) {
                // User pressed button
                usb_host_lib_info_t lib_info;
                ESP_ERROR_CHECK(usb_host_lib_info(&lib_info));
                if (lib_info.num_devices != 0) {
                    ESP_LOGW(TAG, "Shutdown with attached devices.");
                }
                // End while cycle
                break;
            }
        }
    }

    // Deregister client
    class_driver_client_deregister();
    vTaskDelay(10);

    // Delete the tasks
    vTaskDelete(class_driver_task_hdl);
    vTaskDelete(host_lib_task_hdl);

    // Delete interrupt and queue
    gpio_isr_handler_remove(APP_QUIT_PIN);
    xQueueReset(app_event_queue);
    vQueueDelete(app_event_queue);
    ESP_LOGI(TAG, "End of the example");
}
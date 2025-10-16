/*
 * ESP32-S3 USB Host Flash Bridge (Rewritten)
 * Receives firmware from PC via UART and flashes target ESP32 WROOM via USB Host
 * Uses native USB Host Library without CDC-ACM component dependency
 */

#include "usb_handler.h"
#include "rbg_handler.h"

static const char *TAG = "MAIN APP";

QueueHandle_t app_event_queue = NULL;
TaskHandle_t main_task_handle = NULL;

/**
 * @brief APP event group
 *
 * APP_EVENT            - General event, which is APP_QUIT_PIN press event in this example.
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

static void gpio45_isr_handler(void *arg) {
    // Handle GPIO45 interrupt here
    const app_event_queue_t evt_queue = {
        .event_group = APP_EVENT_PUSH,
    };
    BaseType_t xTaskWoken = pdFALSE;
    if (app_event_queue) {
        xQueueSendFromISR(app_event_queue, &evt_queue, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void setup_gpio45_interrupt(void) {
    gpio_config_t gpio45_cfg = {
        .pin_bit_mask = BIT64(45),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
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
void app_main(void)
{
    ESP_LOGI(TAG, "USB host library example");
    init_led_strip();
    led_on();
    setup_gpio45_interrupt();
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
    main_task_handle = xTaskGetCurrentTaskHandle();
    app_event_queue_t evt_queue;
    uint8_t change = 0;

    // Start USB tasks
    // usb_host_lib_task_start();
    // ulTaskNotifyTake(false, 1000); // Wait until the USB host library is installed
    // class_driver_task_start();
    // usb_otg_rw_task_start();
    jtag_task_start();
    jtag_task_stop();

    while (1) {
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT_PUSH == evt_queue.event_group) {
                // User pressed button
                if (change == 0) {
                    change = 1;
                    ESP_LOGI(TAG, "Button pressed, switch to jtag");
                    jtag_task_resume();
                    // usb_otg_rw_task_stop();
                    // usb_host_lib_task_stop();
                    // class_driver_task_stop();
                    led_show_red();
                } else {
                    change = 0;
                    ESP_LOGI(TAG, "Button pressed, switch to USB Host");
                    jtag_task_stop();
                    // usb_host_lib_task_resume();
                    // class_driver_task_resume();
                    // usb_otg_rw_task_resume();
                    led_show_blue();
                }
            }
        }
    }
}
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/queue.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include <esp_timer.h>
#include "GPIO_initialize.h"

static const char *TAG = "GATEWAY DEVICE";

// Input pins
#define GPIO_INPUT_1   32
#define PIN_START   34
#define PIN_STOP  35
#define PIN_IDLE   36
#define PIN_ALARM   39

// Output pins
#define GPIO_OUTPUT_1  21
#define GPIO_OUTPUT_2  22

// Debounce window in microseconds
#define DEBOUNCE_US 50000

// input pwm pins
#define GPIO_INPUT_PIN_SEL_PWM  ((1ULL << GPIO_INPUT_1))
// input pins
#define GPIO_INPUT_PIN_SEL  ((1ULL << PIN_START) |(1ULL << PIN_STOP) |(1ULL << PIN_IDLE) |(1ULL << PIN_ALARM) )
// output pins
#define GPIO_OUTPUT_PIN_SEL ((1ULL << GPIO_OUTPUT_1) | (1ULL << GPIO_OUTPUT_2) )

QueueHandle_t gpio_event_queue = NULL;

// Last accepted edge time per pin, for debounce filtering.
// Sized GPIO_NUM_MAX so it's safely indexable by any valid pin number.
static int64_t last_edge_time[GPIO_NUM_MAX] = {0};

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int pin = (int)(intptr_t)arg;
    int64_t now = esp_timer_get_time();

    // Discard edge if within debounce window of last accepted edge
    if ((now - last_edge_time[pin]) < DEBOUNCE_US) {
        return;
    }
    last_edge_time[pin] = now;

    gpio_event_t evt = {
        .pin = pin,
        .level = gpio_get_level(pin),
        .timestamp_us = now
    };

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(gpio_event_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

 void configure_gpio(void)
{
    gpio_event_queue = xQueueCreate(10, sizeof(gpio_event_t));

    /* --- Configure inputs --- */
    gpio_config_t input_conf = {
        .pin_bit_mask = GPIO_INPUT_PIN_SEL,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   /* pin reads 0 when floating, 1 when driven high */
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&input_conf);

    /* --- Configure inputs for PWM --- */
    gpio_config_t io_conf = {
        .pin_bit_mask = GPIO_INPUT_PIN_SEL_PWM,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   /* pin reads 0 when floating, 1 when driven high */
        .intr_type    = GPIO_INTR_POSEDGE ,     /*for rising edge*/
    };
    gpio_config(&io_conf);

    /* --- Configure outputs --- */
    gpio_config_t output_conf = {
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&output_conf);

    gpio_isr_handler_add(PIN_START, gpio_isr_handler, (void *)(intptr_t)PIN_START);
    gpio_isr_handler_add(PIN_STOP,  gpio_isr_handler, (void *)(intptr_t)PIN_STOP);
    gpio_isr_handler_add(PIN_IDLE,  gpio_isr_handler, (void *)(intptr_t)PIN_IDLE);
    gpio_isr_handler_add(PIN_ALARM,  gpio_isr_handler, (void *)(intptr_t)PIN_ALARM);

ESP_LOGI(TAG, "GPIO input/output pins configured");

}




#ifndef GPIO_INITIALIZE_H
#define GPIO_INITIALIZE_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

// Input pins
#define GPIO_INPUT_1   32
#define PIN_START   34
#define PIN_STOP   35
#define PIN_IDLE   36
#define PIN_ALARM   39

// Output pins
#define GPIO_OUTPUT_1  21
#define GPIO_OUTPUT_2  22

// Event structure sent from ISR to consumer task
typedef struct {
    int pin;
    int level;
    int64_t timestamp_us;
} gpio_event_t;

extern QueueHandle_t gpio_event_queue;

// Function declaration
void configure_gpio(void);

#endif

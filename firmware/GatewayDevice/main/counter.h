#ifndef COUNTER_H
#define COUNTER_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void counter_init(uint32_t pin);
int  counter_get_count(void);
void counter_set_count(uint32_t count);

// Registers a task to be notified on every count increment (press mode only).
// Pass NULL to disable per-count notification (CNC mode).
void counter_register_notify_task(TaskHandle_t task);

#endif

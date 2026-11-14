#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Must be called once from app_main(), before any task subscribes.
void watchdog_init(void);

void watchdog_subscribe(TaskHandle_t task);

#endif

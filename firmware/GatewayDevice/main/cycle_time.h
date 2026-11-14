#ifndef CYCLE_TIME_H
#define CYCLE_TIME_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void cycle_time_task(void *pvParameters);
void cycle_time_register_notify_task(TaskHandle_t task);
void cycle_time_get_start_str(char *buf, size_t buf_len);
void cycle_time_get_stop_str(char *buf, size_t buf_len);
void cycle_time_register_start_callback(void (*cb)(void));
void cycle_time_daily_reset_task(void *pvParameters);

double cycle_time_get_idle_time(void);
double cycle_time_get_last_cycle_duration(void);
uint8_t  cycle_time_get_alarm_state(void);


#endif

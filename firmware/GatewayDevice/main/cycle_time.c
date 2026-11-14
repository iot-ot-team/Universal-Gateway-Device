#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cycle_time.h"
#include "GPIO_initialize.h"
#include "esp_log.h"
#include "esp_event.h"
#include "PWM.h"
#include "counter.h"
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "esp_task_wdt.h"

static const char *TAG = "GATEWAY DEVICE";

static double last_cycle_idle_time;
static double last_cycle_duration;
static uint8_t last_cycle_alarm_state;
static char last_cycle_start_str[32];
static char last_cycle_stop_str[32];
static void (*start_callback)(void) = NULL;

static TaskHandle_t notify_task_handle = NULL;

static bool copy_string_field(char *destination, size_t destination_size, const char *source)
{
    // No buffer or zero capacity: nothing we can safely write
    if (destination == NULL || destination_size == 0) {
        return false;
    }

    // Start empty so the buffer is valid even on early return
    destination[0] = '\0';

    if (source == NULL) {
        return false;
    }

    // strnlen reads at most destination_size bytes of source
    size_t source_length = strnlen(source, destination_size);

    // length < size means the text plus its NUL fits
    bool fits = (source_length < destination_size);

    // Copy all if it fits, otherwise leave 1 byte for the NUL
    size_t copy_size = fits ? source_length : (destination_size - 1);

    // i never goes past destination_size - 2, so no overflow
    for (size_t i = 0; i < copy_size; i++) {
        destination[i] = source[i];
    }

    destination[copy_size] = '\0';
    return fits;
}

void cycle_time_register_notify_task(TaskHandle_t task)
{
    notify_task_handle = task;
}

// State machine states for one machine cycle
typedef enum {
    STATE_WAIT_START,   // machine stopped, waiting for a start edge
    STATE_RUNNING,       // machine running, timing the cycle
    STATE_MACHINE_IDLE   // running, but idle pin currently high
} cycle_state_t;

static void get_current_time_str(char *buf, size_t buf_len)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // tm_year is years since 1900, so 124 == year 2024. If the clock reads
    // earlier than that, SNTP has never synced (ESP32 boots at epoch 0 =
    // 1970 by default) -- so instead of publishing a garbage 1970 date,
    // leave the field blank.
    if (timeinfo.tm_year < 124) {
        buf[0] = '\0';   // empty string -- "no valid time yet"
        return;
    }
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

// ---- NEW: daily 8:00 AM part-count reset ----

static int64_t seconds_until_next_8am(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    struct tm tm_target = tm_now;
    tm_target.tm_hour = 8;
    tm_target.tm_min  = 0;
    tm_target.tm_sec  = 0;

    time_t target = mktime(&tm_target);

    if (target <= now) {
        // 8am already passed today -- target tomorrow instead
        target += 24 * 60 * 60;
    }

    return (int64_t)difftime(target, now);
}

void cycle_time_daily_reset_task(void *pvParameters)
{
    while (1) {
        int64_t wait_s = seconds_until_next_8am();
        ESP_LOGI(TAG, "Daily reset: sleeping %lld s until next 8:00 AM", (long long)wait_s);

        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));

        ESP_LOGI(TAG, "8:00 AM reached -- resetting part count to 0");
        counter_set_count(0);
    }
}

void cycle_time_task(void *pvParameters)
{
    gpio_event_t evt;

    // Current state, starts assuming machine is stopped
    cycle_state_t state = STATE_WAIT_START;

    // esp_timer values (microseconds since boot) — used for precise
    // duration math, since wall-clock time() only has 1-second resolution
    int64_t start_time_esp_us = 0;
    int64_t idle_start_esp_us = 0;
    int64_t idle_accum_esp_us = 0;   // total idle time within current cycle
    uint32_t alarm_count = 0;

    // Human-readable start timestamp, captured once per cycle at START
    char start_time_str[32];

    while (1) {
        // Block indefinitely until an event arrives from the ISR
        BaseType_t got_event = (xQueueReceive(gpio_event_queue, &evt, pdMS_TO_TICKS(10000)) == pdTRUE);

        esp_task_wdt_reset();

        if (!got_event) {
            // Timed out waiting -- no edge came in, loop back and wait again.
            continue;
        }
            // --- START pin ---
        if (evt.pin == PIN_START && evt.level == 1) {
        if (state == STATE_WAIT_START) {

// PRESS counter happens after start

        start_time_esp_us = evt.timestamp_us;
        idle_accum_esp_us = 0;
        alarm_count   = 0;
        state = STATE_RUNNING;

        get_current_time_str(start_time_str, sizeof(start_time_str));

        // Replaces strncpy + manual terminator, the helper always terminates
        copy_string_field(last_cycle_start_str, sizeof(last_cycle_start_str), start_time_str);

        ESP_LOGI(TAG, "Cycle started at %s", start_time_str);

 /*       get_current_time_str(start_time_str, sizeof(start_time_str));
        strncpy(last_cycle_start_str, start_time_str, sizeof(last_cycle_start_str));
        last_cycle_start_str[sizeof(last_cycle_start_str) - 1] = '\0';
        ESP_LOGI(TAG, "Cycle started at %s", start_time_str);
*/
        if (start_callback != NULL) {
            start_callback();   // main.c spawns the right PWM task here
        }
        } else {
        ESP_LOGW(TAG, "Start ignored, cycle already in progress");
        }
}

            // --- ALARM pin: count every rising edge ---
          //  else if (evt.pin == PIN_ALARM && evt.level == 1) {
         //       alarm_count++;
          //      ESP_LOGI(TAG, "Alarm triggered, count this cycle: %lu", alarm_count);
          //  }

            // --- IDLE pin goes HIGH: machine paused mid-cycle ---
            else if (evt.pin == PIN_IDLE && evt.level == 1) {
                if (state == STATE_RUNNING) {
                    idle_start_esp_us = evt.timestamp_us;
                    state = STATE_MACHINE_IDLE;
                    ESP_LOGI(TAG, "Machine went idle");
                }
                // If we get idle-high while in STATE_WAIT_START, there's
                // no active cycle to pause — ignore, nothing to do.
            }

            // --- IDLE pin goes LOW: machine resumes from idle ---
            else if (evt.pin == PIN_IDLE && evt.level == 0) {
                if (state == STATE_MACHINE_IDLE) {
                    idle_accum_esp_us += (evt.timestamp_us - idle_start_esp_us);
                    state = STATE_RUNNING;
                    ESP_LOGI(TAG, "Machine resumed, idle so far: %.2f min",
                             idle_accum_esp_us / 1000000.0 / 60.0);
                }
            }

            // --- STOP pin ---
            else if (evt.pin == PIN_STOP && evt.level == 1) {
                if (state == STATE_RUNNING || state == STATE_MACHINE_IDLE) {

                    // If stop happens while idle is still high, close out
                    // that idle interval first so it's counted correctly.
                    if (state == STATE_MACHINE_IDLE) {
                        idle_accum_esp_us += (evt.timestamp_us - idle_start_esp_us);
                    }
// CNC counter happens at stop

                    int64_t total_us = evt.timestamp_us - start_time_esp_us;
                    int64_t cycle_us = total_us - idle_accum_esp_us;

                    last_cycle_idle_time = (double)(idle_accum_esp_us / 1000000.0 / 60.0);
                    last_cycle_alarm_state = (uint8_t)alarm_count;

                    char stop_time_str[32];
                    get_current_time_str(stop_time_str, sizeof(stop_time_str));

                  //  strncpy(last_cycle_stop_str, stop_time_str, sizeof(last_cycle_stop_str));
                 //   last_cycle_stop_str[sizeof(last_cycle_stop_str) - 1] = '\0';

                     copy_string_field(last_cycle_stop_str, sizeof(last_cycle_stop_str), stop_time_str);

                    if (cycle_us < 0) {
                        // Anomaly: idle_accum_us exceeded total_us — a
                        // timing/logic inconsistency. Flag instead of
                        // logging a nonsense negative duration.
                        ESP_LOGW(TAG, "ANOMALY: computed negative cycle time, discarding reading");
                    } else {
                        double idle_min   = idle_accum_esp_us / 1000000.0 / 60.0 ;
                        double cycle_min  = cycle_us / 1000000.0 / 60.0 ;

                        last_cycle_duration = cycle_min;

                        ESP_LOGI(TAG, "Cycle complete | start=%s stop=%s | idle=%.2f min | cycle=%.2f min",
                                 start_time_str, stop_time_str, idle_min, cycle_min);
                    }

                    // Reset for next cycle
                    state = STATE_WAIT_START;
                    if (notify_task_handle != NULL) {
                        xTaskNotifyGive(notify_task_handle);
                    }
                } else {
                    // Anomaly: stop pressed with no active cycle. Ignore.
                    ESP_LOGW(TAG, "Stop ignored, no cycle in progress");
                }
            }
        }
    }

double cycle_time_get_idle_time(void)           { return last_cycle_idle_time; }
double cycle_time_get_last_cycle_duration(void) { return last_cycle_duration; }
uint8_t  cycle_time_get_alarm_state(void)         { return last_cycle_alarm_state; }

/*
void cycle_time_get_start_str(char *buf, size_t buf_len)
{
    strncpy(buf, last_cycle_start_str, buf_len);
    buf[buf_len - 1] = '\0';
}

void cycle_time_get_stop_str(char *buf, size_t buf_len)
{
    strncpy(buf, last_cycle_stop_str, buf_len);
    buf[buf_len - 1] = '\0';
}
*/

void cycle_time_get_start_str(char *buf, size_t buf_len)
{
    // Old version did buf[buf_len - 1] with no check, so buf_len == 0
    // wrote before the buffer. The helper handles that case.
    copy_string_field(buf, buf_len, last_cycle_start_str);
}

void cycle_time_get_stop_str(char *buf, size_t buf_len)
{
    copy_string_field(buf, buf_len, last_cycle_stop_str);
}

void cycle_time_register_start_callback(void (*cb)(void))
{
    start_callback = cb;
}

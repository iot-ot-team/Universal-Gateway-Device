#include "driver/gpio.h"
#include "esp_log.h"
#include "counter.h"

static const char *TAG = "GATEWAY DEVICE";

volatile uint32_t part_count = 0;
static TaskHandle_t count_notify_task_handle = NULL;

void counter_register_notify_task(TaskHandle_t task)
{
    count_notify_task_handle = task;
}

static void IRAM_ATTR count_isr_handler(void *arg)
{
    part_count++;
    ESP_EARLY_LOGI(TAG, "part_count: %lu", part_count);

    if (count_notify_task_handle != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(count_notify_task_handle, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    }
}

void counter_init(uint32_t pin)
{
    gpio_isr_handler_add(pin, count_isr_handler, NULL);
}

int counter_get_count(void)
{
    return part_count;
}

void counter_set_count(uint32_t count)
{
    portDISABLE_INTERRUPTS();
    part_count = count;
    portENABLE_INTERRUPTS();
}

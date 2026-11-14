#include "watchdog.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "GATEWAY DEVICE";

void watchdog_init(void)
{
 // 10 s = expected task cycle
 // 20 s = watchdog's allowed maximum time
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 20000,
        .idle_core_mask = (1 << 0) | (1 << 1), // also watch both idle tasks
        .trigger_panic = true,                  // timeout -> reset, not just a log
    };

    esp_err_t err = esp_task_wdt_init(&twdt_config);
    if (err != ESP_OK) {
        // Not fatal to halt on -- but if this fails, no subscription
        // below will work either, so make it loud.
        ESP_LOGE(TAG, "esp_task_wdt_init failed: %s", esp_err_to_name(err));
    }
}

void watchdog_subscribe(TaskHandle_t task)
{
    if (task == NULL) {
        // Guards against the common mistake of subscribing before
        // capturing the handle from xTaskCreate (see main.c changes --
        // some tasks currently pass NULL as the handle output).
        ESP_LOGE(TAG, "watchdog_subscribe called with NULL handle");
        return;
    }

    esp_err_t err = esp_task_wdt_add(task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_task_wdt_add failed: %s", esp_err_to_name(err));
    }
}

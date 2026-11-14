#include "GPIO_initialize.h"
#include "counter.h"
#include "PWM.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "eth.h"
#include "MQTT.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "NVS.h"
#include "cycle_time.h"
#include <time.h>
#include <stdlib.h>
#include "web_config.h"
#include "part_count_storage.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "ota.h"

#define BOOT_MODE_PIN GPIO_NUM_4
#define BOOT_MODE_WINDOW_MS 5000

static const char *TAG = "GATE WAY DEVICE";

static QueueHandle_t mqtt_data_queue;   // only created when mode == CNC
static QueueHandle_t mqtt_part_queue;   // only created when mode == PRESS

static device_mode_t g_device_mode;

static bool gateway_is_healthy(void)
{
    return mqtt_is_connected();
}

// consumes gpio_event_queue, decides WHEN to publish (only on START)
// CNC-mode task only
static void gpio_event_task(void *pvParameters)
{
    while (1) {
        BaseType_t got_notification = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

        esp_task_wdt_reset();

        if (!got_notification) {
            continue;
        }

        machine_data_t data = {0};

        cycle_time_get_start_str(data.cycle_start_time, sizeof(data.cycle_start_time));
        cycle_time_get_stop_str(data.cycle_stop_time, sizeof(data.cycle_stop_time));
        data.idle_time        = cycle_time_get_idle_time();
        data.total_cycle_time = cycle_time_get_last_cycle_duration();
        data.count            = counter_get_count();
        data.alarm            = cycle_time_get_alarm_state();

        // buffer to flash only while MQTT is down. During normal
        // operation this is a no-op every cycle -- write only happens
        // on the outage path, keeping flash wear to actual outages.
        if (!mqtt_is_connected()) {
            part_count_storage_write(data.count);
        }

        xQueueSend(mqtt_data_queue, &data, portMAX_DELAY);
    }
}

// blocks on queue, publishes whenever gpio_event_task sends data
// CNC-mode task only
static void mqtt_publish_task(void *pvParameters)
{
    machine_data_t data;
    while (1) {
         BaseType_t got_item = (xQueueReceive(mqtt_data_queue, &data, pdMS_TO_TICKS(10000)) == pdTRUE);

        esp_task_wdt_reset();
        if (got_item) {
            mqtt_publish_machine_data(&data);
        }

    }
}

// press mode only: notified per part count, builds lightweight
// payload, pushes to its own queue. Registered via counter_register_notify_task.
static void press_count_task(void *pvParameters)
{
    while (1) {
        BaseType_t got_notification = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

        esp_task_wdt_reset();

        if (!got_notification) {
            continue;
        }

        part_count_data_t data = {0};
        data.count = counter_get_count();

        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(data.timestamp, sizeof(data.timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

        // same outage-only buffering as CNC mode's gpio_event_task
        if (!mqtt_is_connected()) {
            part_count_storage_write(data.count);
        }

        xQueueSend(mqtt_part_queue, &data, portMAX_DELAY);
    }
}

// press mode only: consumes mqtt_part_queue, publishes each part count
static void mqtt_publish_part_task(void *pvParameters)
{
    part_count_data_t data;
    while (1) {
        BaseType_t got_item = (xQueueReceive(mqtt_part_queue, &data, pdMS_TO_TICKS(10000)) == pdTRUE);

        esp_task_wdt_reset();

        if (got_item) {
            mqtt_publish_part_count(&data);
        }
    }
}

// modbus RTU poll task -- only created if modbus_rtu_enabled
static void modbus_poll_task(void *pvParameters)
{
    const uint32_t poll_every = *(const uint32_t *)pvParameters;
    int energy = 0;

    while (1) {
        if (modbus_master_rtu_read_energy(&energy) == 0) {
            mqtt_publish_energy(energy);
            ESP_LOGI(TAG, "Energy reading: %d", energy);
        } else {
            ESP_LOGE(TAG, "Modbus RTU read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(poll_every));
    }
}

// modbus TCP poll task -- only created if modbus_tcp_enabled
static void modbus_tcp_poll_task(void *pvParameters)
{
    const uint32_t poll_every = *(const uint32_t *)pvParameters;
    int energy = 0;

    while (1) {
        if (modbus_master_tcp_read_energy(&energy) != ESP_OK) {
            mqtt_publish_energy_tcp(energy);
            ESP_LOGE(TAG, "Modbus TCP read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(poll_every));
    }
}

// called synchronously from cycle_time_task right after a valid
// START is accepted (before counter_reset runs).
static void on_cycle_start(void)
{
    if (g_device_mode == DEVICE_MODE_CNC) {
        xTaskCreate((TaskFunction_t)PWM_test_run_cnc, "pwm_cnc_task", 4096, NULL, 5, NULL);
    } else {
        static uint32_t press_duration = 60000;
        xTaskCreate(PWM_test_run_press, "pwm_press_task", 4096, &press_duration, 5, NULL);
    }
}

void app_main(void)
{
    gpio_config_t boot_pin_cfg = {
        .pin_bit_mask = (1ULL << BOOT_MODE_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_pin_cfg);

    bool config_mode = false;
    ESP_LOGI("main", "Checking for config-mode button (%d ms window)...", BOOT_MODE_WINDOW_MS);

    TickType_t start_tick = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(BOOT_MODE_WINDOW_MS)) {
        if (gpio_get_level(BOOT_MODE_PIN) == 0) {
            config_mode = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    nvs_storage_init();
    gpio_install_isr_service(0);   // call ONCE globally in the whole project
    watchdog_init();

    if (config_mode) {
        ESP_LOGI("main", "Boot button held -> entering CONFIG MODE");
        const eth_config_t *e1 = nvs_storage_get_eth_config();
        ethernet_module1(e1->ip, e1->gateway, e1->netmask);   // bring up module 1 only -- to serve HTTP
        web_config_start();
        return;
    }
    configure_gpio();

    ESP_ERROR_CHECK(nvs_storage_init());
    const eth_config_t *cfg = nvs_storage_get_eth_config();

    esp_netif_t *eth_netif = ethernet_module1(cfg->ip, cfg->gateway, cfg->netmask);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "Ethernet (module 1) init failed, halting");
        return;
    }
    esp_netif_t *eth_netif2 = NULL;
    bool eth2_enabled = nvs_storage_get_eth2_enabled();
    if (eth2_enabled) {
        const eth2_config_t *cfg2 = nvs_storage_get_eth2_config();
        eth_netif2 = ethernet_module2(cfg2->ip, cfg2->gateway, cfg2->netmask);
        if (eth_netif2 == NULL) {
            ESP_LOGE(TAG, "Ethernet (module 2) init failed, halting");
            return;
        }
    }

    counter_init(GPIO_INPUT_1);
    part_count_storage_init();

    mqtt_set_ota_cmd_handler(ota_handle_cmd);                 /* MQTT -> OTA (commands) */
    ota_init(gateway_is_healthy, mqtt_publish_ota_status);                       /* OTA  -> MQTT (status)  */

    const mqtt_config_t *mqtt_cfg = nvs_storage_get_mqtt_config();
    mqtt_app_start(mqtt_cfg->broker_ip, mqtt_cfg->broker_port);

    // daily 8am part-count reset -- applies to both CNC and PRESS modes,
    xTaskCreate(cycle_time_daily_reset_task, "daily_reset_task", 4096, NULL, 4, NULL);

    g_device_mode = nvs_storage_get_device_mode();
    bool modbus_rtu_enabled = nvs_storage_get_modbus_rtu_enabled();
    bool modbus_tcp_enabled = nvs_storage_get_modbus_tcp_enabled();

    // modbus RTU init + task, gated by flag
    if (modbus_rtu_enabled) {
        const modbus_rtu_config_t *mbrtu_cfg = nvs_storage_get_rtu_config();
        ESP_ERROR_CHECK(modbus_master_rtu_init(mbrtu_cfg->slave_addr, mbrtu_cfg->baud_rate, mbrtu_cfg->parity));
        xTaskCreate(modbus_poll_task, "modbus_rtu_poll", 4096, (void *)&mbrtu_cfg->poll_every, 3, NULL);
    }

    if (modbus_tcp_enabled) {
        if (!eth2_enabled || eth_netif2 == NULL) {
            ESP_LOGE(TAG, "Modbus TCP enabled but eth2 is not up -- skipping Modbus TCP init");
        } else {
            const modbus_tcp_config_t *mbtcp_cfg = nvs_storage_get_tcp_config();
            ESP_ERROR_CHECK(modbus_master_tcp_init(eth_netif2, mbtcp_cfg->ip, mbtcp_cfg->port, mbtcp_cfg->slave_addr));
            xTaskCreate(modbus_tcp_poll_task, "modbus_tcp_poll", 4096, (void *)&mbtcp_cfg->poll_every, 3, NULL);
        }
    }

    // --- mode-dependent task set ---
    if (g_device_mode == DEVICE_MODE_CNC) {
        mqtt_data_queue = xQueueCreate(10, sizeof(machine_data_t));
        if (mqtt_data_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create MQTT queue");
            return;
        }

        TaskHandle_t gpio_event_task_handle = NULL;
        TaskHandle_t cycle_time_task_handle = NULL;
        TaskHandle_t mqtt_publish_task_handle = NULL;

        xTaskCreate(cycle_time_task, "cycle_time_task", 4096, NULL, 5, &cycle_time_task_handle);
        xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, &mqtt_publish_task_handle);
        xTaskCreate(gpio_event_task, "gpio_event_task", 4096, NULL, 5, &gpio_event_task_handle);

        watchdog_subscribe(cycle_time_task_handle);
        watchdog_subscribe(mqtt_publish_task_handle);
        watchdog_subscribe(gpio_event_task_handle);

        cycle_time_register_notify_task(gpio_event_task_handle);
        cycle_time_register_start_callback(on_cycle_start);

    } else { // DEVICE_MODE_PRESS
        mqtt_part_queue = xQueueCreate(20, sizeof(part_count_data_t));
        if (mqtt_part_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create MQTT part queue");
            return;
        }

        TaskHandle_t press_count_task_handle = NULL;
        TaskHandle_t cycle_time_task_handle = NULL;
        TaskHandle_t mqtt_publish_part_task_handle = NULL;

        xTaskCreate(cycle_time_task, "cycle_time_task", 4096, NULL, 5, &cycle_time_task_handle);
        xTaskCreate(press_count_task, "press_count_task", 4096, NULL, 5, &press_count_task_handle);
        xTaskCreate(mqtt_publish_part_task, "mqtt_publish_part_task", 4096, NULL, 5, &mqtt_publish_part_task_handle);

        watchdog_subscribe(cycle_time_task_handle);
        watchdog_subscribe(press_count_task_handle);
        watchdog_subscribe(mqtt_publish_part_task_handle);

        counter_register_notify_task(press_count_task_handle);
        cycle_time_register_start_callback(on_cycle_start);
    }
}

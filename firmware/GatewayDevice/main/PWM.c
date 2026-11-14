#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "PWM.h"

#define LED_PIN 21
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_HIGH_SPEED_MODE
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LEDC_FREQUENCY 4

#define CNC_HIGH_DURATION_MS 60000   // fixed 1 minute HIGH pulse
#define CNC_LOW_DURATION_MS 10000

static void PWM_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = LED_PIN,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty       = 512,
        .hpoint     = 0
    };
    ledc_channel_config(&ledc_channel);
}

void PWM_test_run_press(void *arg)
{
    uint32_t duration = *(uint32_t *)arg;
    PWM_init();
    vTaskDelay(pdMS_TO_TICKS(duration));
    ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
}

void PWM_test_run_cnc(void)
{
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << LED_PIN),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);

        for (int i = 0; i < 10; i++)
        {
            gpio_set_level(LED_PIN, 1); // HIGH for a full minute
            vTaskDelay(pdMS_TO_TICKS(CNC_HIGH_DURATION_MS));

            gpio_set_level(LED_PIN, 0); // pin just went LOW
            ESP_LOGI("PWM", "CNC test pin LOW - cycle end");
            vTaskDelay(pdMS_TO_TICKS(CNC_LOW_DURATION_MS)); // 10s LOW before next cycle

        }
}

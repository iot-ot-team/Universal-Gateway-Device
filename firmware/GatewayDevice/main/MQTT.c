#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include "counter.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "MQTT.h"
#include "NVS.h"
#include "cycle_time.h"
#include "part_count_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "GATEWAY DEVICE";

static mqtt_cmd_cb_t s_ota_cmd_cb = NULL;
static const char s_ota_cmd_topic[]    = "factory/m1/ota/cmd";     /* gateway listens here   */
static const char s_ota_status_topic[] = "factory/m1/ota/status";  /* gateway reports here   */

static esp_mqtt_client_handle_t client;
static bool s_mqtt_connected = false;

#define PARTCOUNT_REQUEST_TOPIC    "factory/m1/partcount/request"
#define PARTCOUNT_RESPONSE_TOPIC   "factory/m1/partcount/response"
#define PARTCOUNT_MAX_ATTEMPTS     2
#define PARTCOUNT_REPLY_TIMEOUT_MS 10000   // how long to wait for a reply before retrying

static SemaphoreHandle_t s_partcount_reply_sem;   // given by the DATA handler when a reply lands
static uint32_t s_recovered_count = 0;
static bool s_boot_recovery_done = false;         // only run this once per boot, not on every reconnect

void mqtt_set_ota_cmd_handler(mqtt_cmd_cb_t cb)
{
    s_ota_cmd_cb = cb;
}

bool mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

void mqtt_publish_ota_status(const char *json)
{
    if (!client || !s_mqtt_connected) {
        ESP_LOGW(TAG, "Cannot publish OTA status while MQTT is unavailable");
        return;
    }

    if (esp_mqtt_client_publish(client, s_ota_status_topic, json, 0, 1, 0) < 0) {
        ESP_LOGE(TAG, "Failed to publish OTA status");
    }
}

void mqtt_publish_energy(int energy);

/* runs as its own task so it can block/retry without stalling the MQTT event loop */
static void partcount_recovery_task(void *arg)
{
    for (int attempt = 1; attempt <= PARTCOUNT_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Requesting last part count (attempt %d/%d)", attempt, PARTCOUNT_MAX_ATTEMPTS);
        esp_mqtt_client_publish(client, PARTCOUNT_REQUEST_TOPIC, "{\"request\":\"last_count\"}", 0, 1, 0);

        if (xSemaphoreTake(s_partcount_reply_sem, pdMS_TO_TICKS(PARTCOUNT_REPLY_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "Recovered count from DB: %u", (unsigned)s_recovered_count);
            counter_set_count(s_recovered_count);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "No reply on attempt %d", attempt);
    }

    // both attempts failed -- give up, start counting from 0
    ESP_LOGE(TAG, "No part count reply after %d attempts, resetting to 0", PARTCOUNT_MAX_ATTEMPTS);
    counter_set_count(0);

    vTaskDelete(NULL);
}

/* MQTT EVENT HANDLER */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT CONNECTED");
        s_mqtt_connected = true;

        if (esp_mqtt_client_subscribe(event->client, s_ota_cmd_topic, 0) < 0) {
            ESP_LOGE(TAG, "Failed to subscribe to OTA command topic %s", s_ota_cmd_topic);
        }

        esp_mqtt_client_subscribe(client, PARTCOUNT_RESPONSE_TOPIC, 1);

        // --- Unified recovery: NVS/raw partition is ALWAYS checked first ---
        // This runs on every connect event (first boot AND every reconnect),
        // because if the device rebooted while disconnected, or briefly
        // dropped link, the partition may hold counts that never reached MQTT.
        uint32_t recovered_count = 0;
        esp_err_t err = part_count_storage_get_last(&recovered_count);

        if (err == ESP_OK) {
            // NVS had a buffered value -- this wins, no matter what. DB is
            // never consulted when NVS has data (DB resets daily at 8AM,
            // so comparing/maxing the two would break on reset days).
            counter_set_count(recovered_count);
            ESP_LOGI(TAG, "Counter recovered from NVS: %u", (unsigned)recovered_count);

            // Clear it now so this same value isn't picked up again on the
            // next reconnect (i.e. don't let it "reapply" itself later).
            part_count_storage_clear();

            s_boot_recovery_done = true;   // NVS satisfied recovery; no DB query needed

        } else if (err == ESP_ERR_NOT_FOUND) {
            // NVS is empty. Only fall back to DB if this is the very first
            // connect after power-up -- a mid-runtime reconnect with empty
            // NVS just means nothing happened while disconnected, so the
            // live in-RAM counter is already correct and should be left alone.
            if (!s_boot_recovery_done) {
                s_boot_recovery_done = true;
                ESP_LOGI(TAG, "No NVS data on first boot, falling back to DB");
                xTaskCreate(partcount_recovery_task, "partcount_recovery", 4096, NULL, 5, NULL);
            } else {
                ESP_LOGI(TAG, "Reconnected, nothing buffered in NVS -- counter unchanged");
            }

        } else {
            // Real read error (not just "empty"). Don't guess -- leave the
            // counter as-is rather than risk overwriting a good value.
            ESP_LOGE(TAG, "NVS read error: %s -- counter left unchanged", esp_err_to_name(err));
        }

        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT DISCONNECTED");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MESSAGE SENT");
        break;

 /*   case MQTT_EVENT_DATA:
        if (event->topic &&
            event->topic_len == (int)strlen(s_ota_cmd_topic) &&
            strncmp(event->topic, s_ota_cmd_topic, event->topic_len) == 0) {
            if (event->current_data_offset == 0 &&
                event->data_len == event->total_data_len &&
                s_ota_cmd_cb) {
                s_ota_cmd_cb(event->data, event->data_len);
            } else {
                ESP_LOGW(TAG, "Ignoring fragmented or unhandled OTA command");
            }
            break;
        }

        if (event->topic_len == strlen(PARTCOUNT_RESPONSE_TOPIC) &&
            strncmp(event->topic, PARTCOUNT_RESPONSE_TOPIC, event->topic_len) == 0) {

            char payload[64] = {0};
            int len = event->data_len < (int)sizeof(payload) - 1 ? event->data_len : (int)sizeof(payload) - 1;
            memcpy(payload, event->data, len);

            uint32_t count = 0;
            if (sscanf(payload, "{\"count\":%" SCNu32 "}", &count) == 1) {
                s_recovered_count = count;
                xSemaphoreGive(s_partcount_reply_sem);
            } else {
                ESP_LOGW(TAG, "Could not parse part count reply: %s", payload);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT ERROR");
        break;

    default:
        break;
    }
}

void mqtt_publish_energy(int energy)
{
    char msg[64];
    sprintf(msg, "{\"energy\":%d}", energy);
    esp_mqtt_client_publish(client, "factory/m1/energy", msg, 0, 1, 0);
}

void mqtt_publish_energy_tcp(int energy)
{
    char msg[64];
    sprintf(msg, "{\"energy\":%d}", energy);
    esp_mqtt_client_publish(client, "factory/m1/energy_tcp", msg, 0, 1, 0);
}
*/

    case MQTT_EVENT_DATA:
        if (event->topic &&
            event->topic_len == (int)strlen(s_ota_cmd_topic) &&
            strncmp(event->topic, s_ota_cmd_topic, event->topic_len) == 0) {
            if (event->current_data_offset == 0 &&
                event->data_len == event->total_data_len &&
                s_ota_cmd_cb) {
                s_ota_cmd_cb(event->data, event->data_len);
            } else {
                ESP_LOGW(TAG, "Ignoring fragmented or unhandled OTA command");
            }
            break;
        }

        // Added: event->topic NULL check (the OTA branch above had it, this one did not)
        // sizeof() on a string literal macro gives length + 1 (for the NUL), so
        // sizeof(...) - 1 is the text length, computed at compile time with no strlen call
        if (event->topic &&
            event->topic_len == (int)(sizeof(PARTCOUNT_RESPONSE_TOPIC) - 1) &&
            strncmp(event->topic, PARTCOUNT_RESPONSE_TOPIC, event->topic_len) == 0) {

            char payload[64] = {0};

            // Clamp the length to 0 .. sizeof(payload) - 1
            // The negative check protects against a bad data_len, which would
            // otherwise turn into a huge unsigned value in a copy
            int len = event->data_len;
            if (len < 0) {
                len = 0;
            }
            if (len > (int)sizeof(payload) - 1) {
                len = (int)sizeof(payload) - 1;
            }

            // Manual bounded copy (replaces memcpy). i never reaches
            // sizeof(payload) - 1, and payload[len] stays '\0' from the
            // = {0} initializer, so the string is always terminated
            for (int i = 0; i < len; i++) {
                payload[i] = event->data[i];
            }

            uint32_t count = 0;
            if (sscanf(payload, "{\"count\":%" SCNu32 "}", &count) == 1) {
                s_recovered_count = count;
                xSemaphoreGive(s_partcount_reply_sem);
            } else {
                ESP_LOGW(TAG, "Could not parse part count reply: %s", payload);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT ERROR");
        break;

    default:
        break;
    }
}

void mqtt_publish_energy(int energy)
{
    char msg[64];

    // snprintf writes at most sizeof(msg) bytes including the NUL.
    // Worst case text is {"energy":-2147483648} = 22 chars, so 64 is plenty
    snprintf(msg, sizeof(msg), "{\"energy\":%d}", energy);
    esp_mqtt_client_publish(client, "factory/m1/energy", msg, 0, 1, 0);
}

void mqtt_publish_energy_tcp(int energy)
{
    char msg[64];

    snprintf(msg, sizeof(msg), "{\"energy\":%d}", energy);
    esp_mqtt_client_publish(client, "factory/m1/energy_tcp", msg, 0, 1, 0);
}

void mqtt_publish_machine_data(const machine_data_t *data)
{
    char msg[384];
    snprintf(msg, sizeof(msg),
        "{"
        "\n  \"cycle_start\": \"%s\","
        "\n  \"cycle_stop\": \"%s\","
        "\n  \"idle_time_min\": %.2f,"
        "\n  \"total_cycle_time_min\": %.2f,"
        "\n  \"count\": %d,"
        "\n  \"alarm\": %d"
        "\n}",
        data->cycle_start_time,
        data->cycle_stop_time,
        data->idle_time,
        data->total_cycle_time,
        data->count,
        data->alarm);

    esp_mqtt_client_publish(client, "factory/m1/machine_data", msg, 0, 1, 0);
}

void mqtt_publish_part_count(const part_count_data_t *data)
{
    char msg[64];
    int len = snprintf(msg, sizeof(msg),
                       "{\"count\":%" PRIu32 ",\"timestamp\":\"%s\"}",
                       data->count, data->timestamp);

    if (len < 0 || len >= (int)sizeof(msg)) {
        ESP_LOGW(TAG, "Part count JSON truncated or failed (len=%d), not publishing", len);
        return;
    }

    esp_mqtt_client_publish(client, "factory/m1/machine_data", msg, 0, 1, 0);
}
void mqtt_app_start(const char *broker_ip, uint16_t broker_port)
{
    s_partcount_reply_sem = xSemaphoreCreateBinary();

    static char uri[32];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", broker_ip, broker_port);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

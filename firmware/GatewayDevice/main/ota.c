#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"        /* esp_restart() */
#include "esp_ota_ops.h"       /* esp_ota_* and esp_app_desc_t */
#include "esp_partition.h"     /* esp_partition_get_sha256() */
#include "esp_http_client.h"   /* HTTP(S) client used for the download */
#include "cJSON.h"             /* parse the MQTT command */

#include "ota.h"

static const char *TAG = "ota";


#define OTA_USE_TLS              0     /* 0 = plain http:// (no certificate needed) */
#define OTA_REQUIRE_HTTPS        OTA_USE_TLS  /* 1 = reject plain http:// URLs      */
#define OTA_SKIP_CN_CHECK        0     /* 1 = don't compare cert CN with the IP    */
#define OTA_VERIFY_SHA256        1     /* compare image digest with MQTT "sha256"  */
#define OTA_RX_BUF_SIZE          1024  /* download chunk size (heap, not stack)    */
#define OTA_HTTP_TIMEOUT_MS      10000
#define OTA_SELFTEST_TIMEOUT_S   60    /* time the new firmware gets to prove itself */
#define OTA_TASK_STACK           8192  /* TLS handshake needs a big stack          */
#define OTA_TASK_PRIO            3     /* keep BELOW your counting tasks           */

typedef struct {
    char url[256];
    char version[32];
    char sha256[65];   /* 64 hex chars + NUL */
} ota_request_t;

static QueueHandle_t      s_req_q      = NULL;
static ota_health_cb_t    s_health_cb  = NULL;
static ota_status_cb_t    s_status_cb  = NULL;
static volatile bool      s_busy       = false;  /* true while an update runs */

/* Version string of the firmware that is running right now. */
static const char *running_version(void)
{
    static char ver[32] = "unknown";
    esp_app_desc_t desc;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_partition_description(run, &desc) == ESP_OK) {
        strlcpy(ver, desc.version, sizeof(ver));
    }
    return ver;
}

/* Builds Json msg to publish {"event":"ota","status":..,"detail":..,"running":..}.*/
static void report(const char *status, const char *detail)
{
    if (!s_status_cb) return;
    char json[160];
    snprintf(json, sizeof(json),
             "{\"event\":\"ota\",\"status\":\"%s\",\"detail\":\"%s\",\"running\":\"%s\"}",
             status, detail ? detail : "", running_version());
    s_status_cb(json);
}

/* Only letters, digits, '.', '_' , '-' allowed (we echo the version in JSON). */
static bool is_safe_token(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '_' || *s == '-')) return false;
    }
    return true;
}

/* Exactly 64 hex characters. */
static bool is_hex64(const char *s)
{
    // strnlen stops counting after 65 bytes, so a very long or unterminated
    // string is never scanned past that point.
    // Result is 64 only if the string is exactly 64 chars long.
    if (strnlen(s, 65) != 64) return false;

    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// converts '7' to 7 and 'a' to a
static uint8_t nibble(char c)
{
    c = (char)tolower((unsigned char)c);
    return (c <= '9') ? (uint8_t)(c - '0') : (uint8_t)(c - 'a' + 10);
}

/* "ab12..." (64 chars) -> 32 raw bytes */
static void hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
}

static bool url_allowed(const char *url)
{
#if OTA_USE_TLS
    if (strncmp(url, "https://", 8) == 0) return true;   /* needs the embedded cert */
#endif
#if !OTA_REQUIRE_HTTPS
    if (strncmp(url, "http://", 7) == 0) return true;
#endif
    return false;
}

/* ------------------------------------------------------------------ */
/* 1. Command entry point (runs in the MQTT task - keep it short)     */
/* ------------------------------------------------------------------ */
void ota_handle_cmd(const char *data, int len)
{
    if (!s_req_q) return;

    if (s_busy) {                       /* an update is already running */
        report("rejected", "busy");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (!root) {
        report("rejected", "bad_json");
        return;
    }

    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *ju = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(root, "sha256");

    ota_request_t req = {0};

    // strnlen(x, N) never counts past N bytes.
    // If the result is < N, the string plus its NUL fits in a buffer of size N.
    // If the result equals N, the string is too long (or has no NUL in range),
    // so the check fails. This is the same rule as the old
    // strlen(x) < sizeof(buffer), with a hard read limit added.
    bool ok = cJSON_IsString(jv) && jv->valuestring &&
              cJSON_IsString(ju) && ju->valuestring &&
              cJSON_IsString(jh) && jh->valuestring &&
              strnlen(jv->valuestring, sizeof(req.version)) < sizeof(req.version) &&
              strnlen(ju->valuestring, sizeof(req.url))     < sizeof(req.url) &&
              strnlen(jh->valuestring, sizeof(req.sha256))  < sizeof(req.sha256);

    if (ok) {
        strlcpy(req.version, jv->valuestring, sizeof(req.version));
        strlcpy(req.url,     ju->valuestring, sizeof(req.url));
        strlcpy(req.sha256,  jh->valuestring, sizeof(req.sha256));
    }
    cJSON_Delete(root);              /* req holds its own copies */

    if (!ok) {
        report("rejected", "missing_field");
        return;
    }
    if (!is_safe_token(req.version) || !is_hex64(req.sha256)) {
        report("rejected", "bad_field");
        return;
    }
    if (!url_allowed(req.url)) {
        report("rejected", "https_required");
        return;
    }

    /* Hand over to ota_task. Timeout 0 = never block the MQTT task. */
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        report("rejected", "busy");
        return;
    }
    report("received", "queued");
}

/* ------------------------------------------------------------------ */
/* 2. The actual download + flash                                     */
/* ------------------------------------------------------------------ */
/* Returns true if the new image is written, verified and set as boot */
static bool do_update(const ota_request_t *req, const char **why)
{
    /* All variables declared up-front because of the goto cleanup below. */
    bool ok = false;
    bool ota_open = false;                 /* true between esp_ota_begin and esp_ota_end */
    esp_ota_handle_t handle = 0;
    esp_http_client_handle_t client = NULL;
    uint8_t *buf = NULL;
    const esp_partition_t *part = NULL;
    int content_len = 0;
    int idle_retries = 0;
    uint8_t want[32];
    uint8_t got[32];
    esp_err_t err;

    hex_to_bytes(req->sha256, want, sizeof(want));

    /* The slot that is NOT running (ota_0 <-> ota_1 round-robin). */
    part = esp_ota_get_next_update_partition(NULL);
    if (!part) { *why = "no_partition"; return false; }

    esp_http_client_config_t cfg = {
        .url               = req->url,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    client = esp_http_client_init(&cfg);
    buf    = malloc(OTA_RX_BUF_SIZE);
    if (!client || !buf) { *why = "no_memory"; goto cleanup; }

    /* Connect (TLS handshake happens here) and read the HTTP headers. */
    if (esp_http_client_open(client, 0) != ESP_OK) { *why = "connect"; goto cleanup; }
    content_len = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) { *why = "http_status"; goto cleanup; }
    if (content_len > (int)part->size) { *why = "too_big"; goto cleanup; }

    /* Erase the target slot gradually while writing (avoids a long erase
      that could trigger the task watchdog). */
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        *why = "ota_begin";
        goto cleanup;
    }
    ota_open = true;

    /* Download loop: read a chunk, write it to flash. */
    for (;;) {
        int n = esp_http_client_read(client, (char *)buf, OTA_RX_BUF_SIZE);
        if (n < 0) {
            /* timeout without data: retry a few times, then give up */
            if (n == -ESP_ERR_HTTP_EAGAIN && ++idle_retries < 5) continue;
            *why = "download";
            goto cleanup;
        }
        if (n == 0) {
            /* 0 = connection closed. Fine only if we got everything. */
            if (esp_http_client_is_complete_data_received(client)) break;
            *why = "download";
            goto cleanup;
        }
        idle_retries = 0;
        if (esp_ota_write(handle, buf, (size_t)n) != ESP_OK) { *why = "ota_write"; goto cleanup; }
    }

    /* esp_ota_end() validates the image (magic byte, checksum, appended
      SHA-256) and frees the handle whatever the result. */
    ota_open = false;
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        *why = (err == ESP_ERR_OTA_VALIDATE_FAILED) ? "bad_image" : "ota_end";
        goto cleanup;
    }

#if OTA_VERIFY_SHA256
    /* For an app partition this returns the SHA-256 that is appended to the
      image. ota_push.py puts the same value in the MQTT message, so a
      mismatch means "this is not the file you meant to send". */
    if (esp_partition_get_sha256(part, got) != ESP_OK) { *why = "sha_read"; goto cleanup; }
    if (memcmp(got, want, sizeof(got)) != 0)           { *why = "sha_mismatch"; goto cleanup; }
#else
    (void)got; (void)want;
#endif

    /* Point otadata at the new slot. With rollback enabled it is marked
     * ESP_OTA_IMG_NEW and gets exactly ONE boot to confirm itself. */
    if (esp_ota_set_boot_partition(part) != ESP_OK) { *why = "set_boot"; goto cleanup; }
    ok = true;

cleanup:
    if (ota_open) esp_ota_abort(handle);      /* throw away a half-written image */
    free(buf);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ok;
}

/* Waits for requests and runs them one at a time. */
static void ota_task(void *arg)
{
    ota_request_t req;

    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;

        if (strcmp(req.version, running_version()) == 0) {
            report("skipped", "same_version");
            s_busy = false;
            continue;
        }

        ESP_LOGI(TAG, "Update to %s requested", req.version);
        report("downloading", NULL);

        const char *why = "unknown";
        if (do_update(&req, &why)) {
            ESP_LOGI(TAG, "New image ready, rebooting");
            report("rebooting", NULL);
            vTaskDelay(pdMS_TO_TICKS(2000));   /* let the MQTT message leave */
            esp_restart();
        }

        ESP_LOGE(TAG, "Update failed: %s", why);
        report("failed", why);
        s_busy = false;
    }
}

/* ------------------------------------------------------------------ */
/* 3. Boot-time self-test and rollback decision                       */
/* ------------------------------------------------------------------ */
static void boot_task(void *arg)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool pending = run &&
                   esp_ota_get_state_partition(run, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;

    if (pending) {
        /* First boot after an update: this image has not been confirmed yet.
         * Reset/power loss before confirming = bootloader rolls back by itself. */
        ESP_LOGW(TAG, "New firmware, running self-test (%d s)", OTA_SELFTEST_TIMEOUT_S);

        for (int s = 0; s < OTA_SELFTEST_TIMEOUT_S; s++) {
            if (s_health_cb && s_health_cb()) {
                esp_ota_mark_app_valid_cancel_rollback();   /* keep this firmware */
                ESP_LOGI(TAG, "Self-test passed, firmware confirmed");
                report("success", "new_firmware_confirmed");
                vTaskDelete(NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* Self-test failed -> go back to the previous slot and reboot. */
        ESP_LOGE(TAG, "Self-test FAILED, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();

        /* Only reached if there is no valid app to go back to. Staying in
         * PENDING_VERIFY would block future updates, so confirm and carry on. */
        ESP_LOGE(TAG, "Rollback impossible, keeping current firmware");
        esp_ota_mark_app_valid_cancel_rollback();
        vTaskDelete(NULL);
    }

    /* Normal boot. If a previous update was rolled back, tell the backend
     * once MQTT is up. (The invalid slot stays marked until it is
     * overwritten by the next update, so this repeats on each boot until then.) */
    if (esp_ota_get_last_invalid_partition() != NULL) {
        for (int s = 0; s < OTA_SELFTEST_TIMEOUT_S; s++) {
            if (s_health_cb && s_health_cb()) {
                report("rolled_back", "previous_update_failed");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 4. Init                                                            */
/* ------------------------------------------------------------------ */
void ota_init(ota_health_cb_t health_cb, ota_status_cb_t status_cb)
{
    s_health_cb = health_cb;
    s_status_cb = status_cb;

    s_req_q = xQueueCreate(1, sizeof(ota_request_t));   /* one pending request max */
    configASSERT(s_req_q);

    xTaskCreate(ota_task,  "ota",      OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL);
    xTaskCreate(boot_task, "ota_boot", 4096,           NULL, OTA_TASK_PRIO, NULL);
}

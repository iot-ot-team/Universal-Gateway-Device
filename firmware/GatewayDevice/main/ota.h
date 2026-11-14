#pragma once
#include <stdbool.h>

typedef bool (*ota_health_cb_t)(void);

/* Publishes a small JSON status string to <device>/ota/status.
  Implemented by mqtt.c, passed in by main.c. */
typedef void (*ota_status_cb_t)(const char *json);

/* Creates the request queue + the two OTA tasks.
  Call once from app_main(), BEFORE mqtt starts, so no command can arrive
  before the queue exists. */
void ota_init(ota_health_cb_t health_cb, ota_status_cb_t status_cb);

/* Feed the raw payload of an MQTT message received on <device>/ota/cmd.
  Expected JSON: {"version":"1.0.1","url":"https://ip:8443/x.bin","sha256":"<64 hex>"} */
void ota_handle_cmd(const char *data, int len);

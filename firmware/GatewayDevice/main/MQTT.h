#ifndef MQTT_H
#define MQTT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char cycle_start_time[32];
    char cycle_stop_time[32];
    double idle_time;
    double total_cycle_time;
    int      count;
    uint8_t  alarm;
} machine_data_t;

void mqtt_app_start(const char *broker_ip, uint16_t broker_port);
void mqtt_publish_energy(int energy);
void mqtt_publish_energy_tcp(int energy);
void mqtt_publish_machine_data(const machine_data_t *data);
bool mqtt_is_connected(void);

typedef struct {
    uint32_t count;
    char     timestamp[20];   // "YYYY-MM-DD HH:MM:SS"
} part_count_data_t;

void mqtt_publish_part_count(const part_count_data_t *data);

typedef void (*mqtt_cmd_cb_t)(const char *data, int len);

void mqtt_set_ota_cmd_handler(mqtt_cmd_cb_t cb);   /* who handles <device>/ota/cmd   */
void mqtt_publish_ota_status(const char *json);    /* publishes to <device>/ota/status */

#endif

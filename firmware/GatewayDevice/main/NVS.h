#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H
#include "modbus_rtu.h"

#include "esp_err.h"
#include <stdint.h>
#include "driver/uart.h"

typedef struct {
    char ip[16];
    char gateway[16];
    char netmask[16];
} eth_config_t;

typedef struct {
    char ip[16];
    char gateway[16];
    char netmask[16];
} eth2_config_t;

typedef struct {
    uint8_t  slave_addr;
    uint32_t poll_every;
    uint32_t baud_rate;
    uart_parity_t parity;
} modbus_rtu_config_t;

typedef struct {
    char     ip[16];
    uint16_t port;
    uint8_t  slave_addr;
    uint32_t poll_every;
} modbus_tcp_config_t;

typedef struct {
    char     broker_ip[16];
    uint16_t broker_port;
} mqtt_config_t;

typedef struct {
    char name[32];   // machine name, adjust size if you need longer names
} device_config_t;

// Must be called once in main.c, before eth_init() and before any other
// domain init that needs config from flash.
esp_err_t nvs_storage_init(void);

// Returns a pointer to the loaded eth config. Never NULL after
// nvs_storage_init() has succeeded — falls back to defaults if
// nothing was saved in flash yet.
const eth_config_t* nvs_storage_get_eth_config(void);
const eth2_config_t* nvs_storage_get_eth2_config(void);
const modbus_rtu_config_t* nvs_storage_get_rtu_config(void);
const modbus_tcp_config_t *nvs_storage_get_tcp_config(void);
const mqtt_config_t* nvs_storage_get_mqtt_config(void);
const device_config_t* nvs_storage_get_device_config(void);

// Called later by http_server.c once the config page exists.
// Saves to flash AND updates the in-memory struct returned above.
esp_err_t nvs_storage_save_eth_config(const eth_config_t *cfg);
esp_err_t nvs_storage_save_eth2_config(const eth2_config_t *cfg);
esp_err_t nvs_storage_save_rtu_config(const modbus_rtu_config_t *cfg);
esp_err_t nvs_storage_save_tcp_config(const modbus_tcp_config_t *cfg);
esp_err_t nvs_storage_save_mqtt_config(const mqtt_config_t *cfg);
esp_err_t nvs_storage_save_device_config(const device_config_t *cfg);

typedef enum {
    DEVICE_MODE_CNC = 0,
    DEVICE_MODE_PRESS = 1
} device_mode_t;

device_mode_t nvs_storage_get_device_mode(void);
esp_err_t nvs_storage_save_device_mode(device_mode_t mode);

bool nvs_storage_get_eth2_enabled(void);
esp_err_t nvs_storage_save_eth2_enabled(bool enabled);

bool nvs_storage_get_modbus_rtu_enabled(void);
esp_err_t nvs_storage_save_modbus_rtu_enabled(bool enabled);

bool nvs_storage_get_modbus_tcp_enabled(void);
esp_err_t nvs_storage_save_modbus_tcp_enabled(bool enabled);

#endif

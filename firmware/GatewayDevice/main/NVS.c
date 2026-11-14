#include "NVS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_storage";
static const char *NVS_NAMESPACE = "gwconfig";

// In-memory copy, loaded once at boot. This is what the getter
// pointer refers to -- domain files never talk to nvs.h directly.
static eth_config_t s_eth_cfg;
static eth2_config_t s_eth2_cfg;
static modbus_rtu_config_t s_modbus_rtu_cfg;
static modbus_tcp_config_t s_modbus_tcp_cfg;
static mqtt_config_t s_mqtt_cfg;

// Hardcoded fallback -- used only if nothing exists in flash yet
// (first boot, or a fresh chip)

// ---------- ETH MODULE 1 ----------

static void load_eth_defaults(eth_config_t *cfg)
{
    strcpy(cfg->ip,      "192.168.54.205");
    strcpy(cfg->gateway, "192.168.54.1");
    strcpy(cfg->netmask, "255.255.255.0");
}

static esp_err_t load_eth_config_from_nvs(eth_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len;
    len = sizeof(cfg->ip);
    err = nvs_get_str(handle, "eth_ip", cfg->ip, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = sizeof(cfg->gateway);
    err = nvs_get_str(handle, "eth_gw", cfg->gateway, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = sizeof(cfg->netmask);
    err = nvs_get_str(handle, "eth_mask", cfg->netmask, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_eth_config(const eth_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "eth_ip",   cfg->ip);
    nvs_set_str(handle, "eth_gw",   cfg->gateway);
    nvs_set_str(handle, "eth_mask", cfg->netmask);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_eth_cfg, cfg, sizeof(s_eth_cfg)); // keep in-memory copy in sync
    }
    return err;
}

const eth_config_t* nvs_storage_get_eth_config(void)
{
    return &s_eth_cfg;
}

// ---------- ETH MODULE 2 ----------

static void load_eth2_defaults(eth2_config_t *cfg)
{
    strcpy(cfg->ip,      "192.168.54.206");   // adjust if module 2 needs a distinct default
    strcpy(cfg->gateway, "192.168.54.1");
    strcpy(cfg->netmask, "255.255.255.0");
}

static esp_err_t load_eth2_config_from_nvs(eth2_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len;
    len = sizeof(cfg->ip);
    err = nvs_get_str(handle, "eth2_ip", cfg->ip, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = sizeof(cfg->gateway);
    err = nvs_get_str(handle, "eth2_gw", cfg->gateway, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = sizeof(cfg->netmask);
    err = nvs_get_str(handle, "eth2_mask", cfg->netmask, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }


    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_eth2_config(const eth2_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "eth2_ip", cfg->ip);
    nvs_set_str(handle, "eth2_gw", cfg->gateway);
    nvs_set_str(handle, "eth2_mask", cfg->netmask);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_eth2_cfg, cfg, sizeof(s_eth2_cfg));
    }
    return err;
}

const eth2_config_t* nvs_storage_get_eth2_config(void)
{
    return &s_eth2_cfg;
}


// ---------- MODBUS RTU (SLAVE ADDRESS + POLL INTERVAL) ----------

static void load_modbus_rtu_defaults(modbus_rtu_config_t *cfg)
{
    cfg->slave_addr = 1;
    cfg->poll_every = 10000;   // ms
    cfg->baud_rate  = 9600;
    cfg->parity = UART_PARITY_DISABLE;
}

static esp_err_t load_rtu_config_from_nvs(modbus_rtu_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(handle, "rtu_addr", &cfg->slave_addr);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u32(handle, "rtu_poll", &cfg->poll_every);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u32(handle, "rtu_baud", &cfg->baud_rate);
    if (err != ESP_OK) {nvs_close(handle);
        return err;
    }

    err = nvs_get_u8(handle, "rtu_parity", (uint8_t *)&cfg->parity);
    if (err != ESP_OK) {nvs_close(handle); return err; }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_rtu_config(const modbus_rtu_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle,  "rtu_addr", cfg->slave_addr);
    nvs_set_u32(handle, "rtu_poll", cfg->poll_every);
    nvs_set_u32(handle, "rtu_baud", cfg->baud_rate);
    nvs_set_u8(handle, "rtu_parity", (uint8_t)cfg->parity);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_modbus_rtu_cfg, cfg, sizeof(s_modbus_rtu_cfg));
    }
    return err;
}

const modbus_rtu_config_t* nvs_storage_get_rtu_config(void)
{
    return &s_modbus_rtu_cfg;
}

// ---------- MODBUS TCP (IP, PORT, SLAVE ADDRESS) ----------

static void load_modbus_tcp_defaults(modbus_tcp_config_t *cfg)
{
    strcpy(cfg->ip, "192.168.54.89");
    cfg->port        = 502;
    cfg->slave_addr  = 1;
    cfg->poll_every  = 2000;   // ms
}

static esp_err_t load_tcp_config_from_nvs(modbus_tcp_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(cfg->ip);
    err = nvs_get_str(handle, "tcp_ip", cfg->ip, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u16(handle, "tcp_port", &cfg->port);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u8(handle, "tcp_addr", &cfg->slave_addr);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u32(handle, "tcp_poll", &cfg->poll_every);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_tcp_config(const modbus_tcp_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "tcp_ip",   cfg->ip);
    nvs_set_u16(handle, "tcp_port", cfg->port);
    nvs_set_u8(handle,  "tcp_addr", cfg->slave_addr);
    nvs_set_u32(handle, "tcp_poll", cfg->poll_every);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_modbus_tcp_cfg, cfg, sizeof(s_modbus_tcp_cfg));
    }
    return err;
}

const modbus_tcp_config_t* nvs_storage_get_tcp_config(void)
{
    return &s_modbus_tcp_cfg;
}

// ---------- MQTT BROKER ----------

static void load_mqtt_defaults(mqtt_config_t *cfg)
{
    strcpy(cfg->broker_ip, "192.168.54.89");
    cfg->broker_port = 1883;
}

static esp_err_t load_mqtt_config_from_nvs(mqtt_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(cfg->broker_ip);
    err = nvs_get_str(handle, "mqtt_ip", cfg->broker_ip, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_u16(handle, "mqtt_port", &cfg->broker_port);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_mqtt_config(const mqtt_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "mqtt_ip",   cfg->broker_ip);
    nvs_set_u16(handle, "mqtt_port", cfg->broker_port);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_mqtt_cfg, cfg, sizeof(s_mqtt_cfg));
    }
    return err;
}

const mqtt_config_t* nvs_storage_get_mqtt_config(void)
{
    return &s_mqtt_cfg;
}

// ---------- MACHINE NAME ----------

static device_config_t s_device_cfg;

static void load_device_defaults(device_config_t *cfg)
{
    strcpy(cfg->name, "MACHINE_1");
}

static esp_err_t load_device_config_from_nvs(device_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(cfg->name);
    err = nvs_get_str(handle, "machine_name", cfg->name, &len);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_device_config(const device_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "machine_name", cfg->name);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&s_device_cfg, cfg, sizeof(s_device_cfg));
    }
    return err;
}

const device_config_t* nvs_storage_get_device_config(void)
{
    return &s_device_cfg;
}

// ---------- DEVICE MODE (CNC / PRESS) ----------

static device_mode_t s_device_mode;

static void load_device_mode_defaults(device_mode_t *mode)
{
    *mode = DEVICE_MODE_CNC;
}

static esp_err_t load_device_mode_from_nvs(device_mode_t *mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(handle, "dev_mode", &raw);
    if (err == ESP_OK) {
        *mode = (device_mode_t)raw;
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_device_mode(device_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "dev_mode", (uint8_t)mode);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_device_mode = mode;
    }
    return err;
}

device_mode_t nvs_storage_get_device_mode(void)
{
    return s_device_mode;
}

// ---------- ETH MODULE 2 ENABLE FLAG ----------

static bool s_eth2_enabled;

static void load_eth2_enabled_defaults(bool *enabled)
{
    *enabled = false;
}

static esp_err_t load_eth2_enabled_from_nvs(bool *enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(handle, "eth2_en", &raw);
    if (err == ESP_OK) {
        *enabled = (raw != 0);
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_eth2_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "eth2_en", (uint8_t)enabled);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_eth2_enabled = enabled;
    }
    return err;
}

bool nvs_storage_get_eth2_enabled(void)
{
    return s_eth2_enabled;
}

// ---------- MODBUS RTU ENABLE FLAG ----------

static bool s_modbus_rtu_enabled;

static void load_modbus_rtu_enabled_defaults(bool *enabled)
{
    *enabled = false;
}

static esp_err_t load_modbus_rtu_enabled_from_nvs(bool *enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(handle, "rtu_en", &raw);
    if (err == ESP_OK) {
        *enabled = (raw != 0);
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_modbus_rtu_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "rtu_en", (uint8_t)enabled);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_modbus_rtu_enabled = enabled;
    }
    return err;
}

bool nvs_storage_get_modbus_rtu_enabled(void)
{
    return s_modbus_rtu_enabled;
}

// ---------- MODBUS TCP ENABLE FLAG ----------

static bool s_modbus_tcp_enabled;

static void load_modbus_tcp_enabled_defaults(bool *enabled)
{
    *enabled = false;
}

static esp_err_t load_modbus_tcp_enabled_from_nvs(bool *enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(handle, "tcp_en", &raw);
    if (err == ESP_OK) {
        *enabled = (raw != 0);
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_storage_save_modbus_tcp_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "tcp_en", (uint8_t)enabled);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_modbus_tcp_enabled = enabled;
    }
    return err;
}

bool nvs_storage_get_modbus_tcp_enabled(void)
{
    return s_modbus_tcp_enabled;
}



// ---------- INIT ----------

esp_err_t nvs_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = load_eth_config_from_nvs(&s_eth_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No eth (module 1) config in flash, using defaults");
        load_eth_defaults(&s_eth_cfg);
        nvs_storage_save_eth_config(&s_eth_cfg);
    }

    err = load_eth2_config_from_nvs(&s_eth2_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No eth2 (module 2) config in flash, using defaults");
        load_eth2_defaults(&s_eth2_cfg);
        nvs_storage_save_eth2_config(&s_eth2_cfg);
    }

    err = load_rtu_config_from_nvs(&s_modbus_rtu_cfg);
    if (err != ESP_OK) {
    ESP_LOGW(TAG, "No rtu config in flash, using defaults");
    load_modbus_rtu_defaults(&s_modbus_rtu_cfg);
    nvs_storage_save_rtu_config(&s_modbus_rtu_cfg);
    }

    err = load_tcp_config_from_nvs(&s_modbus_tcp_cfg);
    if (err != ESP_OK) {
    ESP_LOGW(TAG, "No tcp config in flash, using defaults");
    load_modbus_tcp_defaults(&s_modbus_tcp_cfg);
    nvs_storage_save_tcp_config(&s_modbus_tcp_cfg);
    }

    err = load_mqtt_config_from_nvs(&s_mqtt_cfg);
    if (err != ESP_OK) {
    ESP_LOGW(TAG, "No mqtt config in flash, using defaults");
    load_mqtt_defaults(&s_mqtt_cfg);
    nvs_storage_save_mqtt_config(&s_mqtt_cfg);
}

    err = load_device_config_from_nvs(&s_device_cfg);
    if (err != ESP_OK){
    ESP_LOGW(TAG, "No device name config in flash, using defaults");
    load_device_defaults(&s_device_cfg);
    nvs_storage_save_device_config(&s_device_cfg);
}

    err = load_device_mode_from_nvs(&s_device_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No device mode in flash, using default (CNC)");
        load_device_mode_defaults(&s_device_mode);
        nvs_storage_save_device_mode(s_device_mode);
    }

    err = load_eth2_enabled_from_nvs(&s_eth2_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No eth2 enable flag in flash, using default (off)");
        load_eth2_enabled_defaults(&s_eth2_enabled);
        nvs_storage_save_eth2_enabled(s_eth2_enabled);
    }

    err = load_modbus_rtu_enabled_from_nvs(&s_modbus_rtu_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No modbus RTU enable flag in flash, using default (off)");
        load_modbus_rtu_enabled_defaults(&s_modbus_rtu_enabled);
        nvs_storage_save_modbus_rtu_enabled(s_modbus_rtu_enabled);
    }

    err = load_modbus_tcp_enabled_from_nvs(&s_modbus_tcp_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No modbus TCP enable flag in flash, using default (off)");
        load_modbus_tcp_enabled_defaults(&s_modbus_tcp_enabled);
        nvs_storage_save_modbus_tcp_enabled(s_modbus_tcp_enabled);
    }

    return ESP_OK;
}


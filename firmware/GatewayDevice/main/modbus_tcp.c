#include "modbus_tcp.h"
#include "mbcontroller.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "MODBUS_MASTER";
static void *master_handle = NULL;

enum {
    CID_ENERGY_VALUE = 0,
};

typedef struct {
    uint16_t energy_value;
} modbus_values_t;

static modbus_values_t values;

static mb_parameter_descriptor_t param_table[] = {
    { CID_ENERGY_VALUE,                            /*!< Characteristic cid */
        "energy_value",                            /*!< The key (name) of the parameter */
        "raw",                                     /*!< The physical units of the parameter */
        0,                                         /*!< Slave address of device in the Modbus segment */
       MB_PARAM_INPUT,                             /*!< Type of modbus parameter */
       0,                                          /*!< This is the Modbus register address. This is the 0 based value. */
       1,                                          /*!< Size of mb parameter in registers */
       offsetof(modbus_values_t, energy_value),    /*!< Parameter name (OFFSET in the parameter structure or address of instance) */
      PARAM_TYPE_U16,                              /*!< Float, U8, U16, U32, ASCII, etc. */
      2,                                           /*!< Number of bytes in the parameter. */
      { .opt1 = 0, .opt2 = 0, .opt3 = 0 },         /*!< Parameter options used to check limits and etc. */
      PAR_PERMS_READ_TRIGGER },                    /*!< Access permissions based on mode */
};

// esp-modbus wants a NULL-terminated array of IP strings, not a single
// address -- because TCP master can talk to multiple slaves, each one
// indexed by its position in this array (matched against mb_slave_addr
// in the table above). We only have one slave, so it's a 1-entry table.
static char *slave_ip_table[2];

esp_err_t modbus_master_tcp_init(esp_netif_t *eth_netif, const char *slave_ip,
                              uint16_t tcp_port, uint8_t slave_addr)
{
    slave_ip_table[0] = (char *)slave_ip;
    slave_ip_table[1] = NULL;   // terminator -- required by the stack

    mb_communication_info_t comm = {
    .tcp_opts.mode = MB_TCP,
    .tcp_opts.addr_type = MB_IPV4,
    .tcp_opts.port = tcp_port,
    .tcp_opts.ip_addr_table = (void *)slave_ip_table,
    .tcp_opts.ip_netif_ptr = (void *)eth_netif,
};
    esp_err_t err = mbc_master_create_tcp(&comm, &master_handle);
    if (err != ESP_OK || master_handle == NULL) {
        ESP_LOGE(TAG, "mb controller create failed (0x%x)", err);
        return err;
    }

    param_table[CID_ENERGY_VALUE].mb_slave_addr = slave_addr;

    ESP_ERROR_CHECK(mbc_master_set_descriptor(master_handle, param_table,
                                               sizeof(param_table) / sizeof(param_table[0])));

    err = mbc_master_start(master_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mb controller start failed (0x%x)", err);
        return err;
    }

    ESP_LOGI(TAG, "Modbus TCP master started, target %s:%d", slave_ip, tcp_port);
    return ESP_OK;
}

esp_err_t modbus_master_tcp_read_energy(int *out_value)
{
    uint8_t type = 0;

    esp_err_t err = mbc_master_get_parameter(master_handle, CID_ENERGY_VALUE,
                                              (uint8_t *)&values.energy_value, &type);
    if (err == ESP_OK) {
        *out_value = (int)values.energy_value;
        ESP_LOGI(TAG, "energy_value = %d", *out_value);
    } else {
        ESP_LOGW(TAG, "read failed for energy_value (0x%x)", err);
    }
    return err;
}

// tcp_port: Modbus TCP standard is 502.
// slave_addr: NOT a wire address here -- it's a 1-based index into the
// internal IP table (see modbus_master.c). With one slave, this is just 1.

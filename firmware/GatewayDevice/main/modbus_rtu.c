#include "modbus_rtu.h"
#include "mbcontroller.h"   // esp-modbus (FreeModbus-based) master API
#include "driver/uart.h"
#include "esp_log.h"


#define MB_UART_PORT   1
#define MB_TX_PIN      17
#define MB_RX_PIN      16
#define MB_RTS_PIN     4

static const char *TAG = "GATEWAY DEVICE";

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

esp_err_t modbus_master_rtu_init(uint8_t slave_addr, uint32_t baud_rate, uart_parity_t parity)
{
    mb_communication_info_t comm = {
        .ser_opts.port = MB_UART_PORT,
        .ser_opts.mode = MB_RTU,
        .ser_opts.baudrate = baud_rate,
        .ser_opts.parity = parity,
        .ser_opts.data_bits = UART_DATA_8_BITS,
        .ser_opts.stop_bits = UART_STOP_BITS_1,
        .ser_opts.uid = 0,
        .ser_opts.response_tout_ms = 1000,
    };

    esp_err_t err = mbc_master_create_serial(&comm, &master_handle);
    if (err != ESP_OK || master_handle == NULL) {
        ESP_LOGE(TAG, "mb controller create failed (0x%x)", err);
        return err;
    }

    ESP_ERROR_CHECK(uart_set_pin(MB_UART_PORT, MB_TX_PIN, MB_RX_PIN,
                                  MB_RTS_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(MB_UART_PORT, UART_MODE_RS485_HALF_DUPLEX));

    param_table[CID_ENERGY_VALUE].mb_slave_addr = slave_addr;

    ESP_ERROR_CHECK(mbc_master_set_descriptor(master_handle, param_table,
                                               sizeof(param_table) / sizeof(param_table[0])));

    err = mbc_master_start(master_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mb controller start failed (0x%x)", err);
        return err;
    }

    ESP_LOGI(TAG, "Modbus master started on UART%d @ %lu baud", MB_UART_PORT, (unsigned long)baud_rate);
    return ESP_OK;
}

esp_err_t modbus_master_rtu_read_energy(int *out_value)
{
    const mb_parameter_descriptor_t *param = NULL;
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


#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include "esp_err.h"
#include <stdint.h>
#include "driver/uart.h"

esp_err_t modbus_master_rtu_init(uint8_t slave_addr, uint32_t baud_rate, uart_parity_t parity); // new

esp_err_t modbus_master_rtu_read_energy(int *out_value);


#endif

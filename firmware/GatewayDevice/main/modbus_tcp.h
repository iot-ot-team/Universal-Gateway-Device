#ifndef MODBUS_TCP_H
#define MODBUS_TCP_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>

esp_err_t modbus_master_tcp_init(esp_netif_t *eth_netif, const char *slave_ip,
                              uint16_t tcp_port, uint8_t slave_addr);

esp_err_t modbus_master_tcp_read_energy(int *out_value);

#endif

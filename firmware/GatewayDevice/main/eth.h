#ifndef ETH_H
#define ETH_H

#include "esp_netif.h"
#include "driver/spi_master.h"

esp_netif_t *ethernet_module1(const char *ip_str, const char *gw_str, const char *netmask_str);
esp_netif_t *ethernet_module2(const char *ip_str, const char *gw_str, const char *netmask_str);

#endif

#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "esp_err.h"

// Starts the HTTP config server. Call this ONLY in config mode
// (boot button held), after eth_init() has brought up the network.
// main.c owns calling this -- this file does not bring up Ethernet itself.
esp_err_t web_config_start(void);

// Stops the HTTP config server, if ever needed (not required for the
// reboot-based flow, since a reboot tears everything down anyway).
void web_config_stop(void);

#endif // WEB_CONFIG_H

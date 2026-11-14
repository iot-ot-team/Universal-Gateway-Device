
idf_component_register(
    SRCS
        "GPIO_initialize.c"
        "counter.c"
        "eth.c"
        "PWM.c"
        "MQTT.c"
        "modbus_tcp.c"
        "modbus_rtu.c"
        "NVS.c"
        "cycle_time.c"
        "web_config.c"
        "part_count_storage.c"
        "watchdog.c"
        "ota.c"
        "main.c"


    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_timer
        esp_eth
        esp_netif
        esp_event
        nvs_flash
        mqtt
        lwip
        esp_http_server
        esp_system
        app_update
        esp_http_client
        esp_partition
        espressif__cjson



)


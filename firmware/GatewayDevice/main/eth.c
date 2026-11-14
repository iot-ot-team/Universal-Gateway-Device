#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "eth.h"
#include "NVS.h"
#include "esp_sntp.h"   // ESP-IDF SNTP client
#include "esp_netif_sntp.h"
#include <time.h>
#include "freertos/semphr.h"

static const char *TAG = "GATEWAY DEVICE";

typedef struct {
    spi_host_device_t spi_host;
    int pin_sck;
    int pin_miso;
    int pin_mosi;
    int pin_cs;
    int pin_int;
    int pin_rst;
} eth_spi_pins_t;

static const eth_spi_pins_t ETH1_PINS = {
    .spi_host = SPI2_HOST,
    .pin_sck  = 14,
    .pin_miso = 12,
    .pin_mosi = 13,
    .pin_cs   = 15,
    .pin_int  = 26,
    .pin_rst  = 33,
};

static const eth_spi_pins_t ETH2_PINS = {
    .spi_host = SPI3_HOST,
    .pin_sck  = 18,
    .pin_miso = 19,
    .pin_mosi = 23,
    .pin_cs   = 5,
    .pin_int  = 27,
    .pin_rst  = 25,
};

// Locally-administered MACs (bit 1 of the first byte set = 0x02).
// The two W5500 modules must NEVER share a MAC, or the switch will
// see one address flapping between two ports.
static const uint8_t ETH1_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x01, 0x01};
static const uint8_t ETH2_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x02, 0x01};

static bool s_globals_ready = false;
static SemaphoreHandle_t s_sntp_mutex = NULL;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) ESP_LOGI(TAG, "Ethernet Link Up");
    if (id == ETHERNET_EVENT_DISCONNECTED) ESP_LOGI(TAG, "Ethernet Link Down");
}

static void sntp_retry_task(void *arg);
static void sntp_time_init(void);

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    // Every time we get an IP -- first boot connect OR any later reconnect
    // after a link drop -- try SNTP again. Spawned as its own task instead
    // of calling sntp_time_init() directly here, because this handler runs
    // in the system event task context: blocking it for up to 15s (SNTP's
    // wait loop) would stall other queued ETH/IP events behind it.
    xTaskCreate(sntp_retry_task, "sntp_retry", 4096, NULL, 5, NULL);
}

static bool parse_ip_string(const char *ip_str, esp_ip4_addr_t *out_addr)
{
    unsigned int a, b, c, d;

    int matched = sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d);
    if (matched != 4) {
        return false;
    }

    // 8 bit max 255
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }

    out_addr->addr = ESP_IP4TOADDR(a, b, c, d);
    return true;
}

static esp_netif_t *eth_init(const char *ip_str, const char *gw_str,
                             const char *netmask_str,
                             const eth_spi_pins_t *pins,
                             const uint8_t *mac_addr)
{
    if (!s_globals_ready) {
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_handler, NULL));

        // Mutex created once here -- protects sntp_time_init() so
        // module1 and module2 getting IP around the same time can't both
        // touch the SNTP client at once.
        s_sntp_mutex = xSemaphoreCreateMutex();

        s_globals_ready = true;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    esp_netif_ip_info_t ip_info;
    if (!parse_ip_string(ip_str, &ip_info.ip)) {
        ESP_LOGE(TAG, "Invalid IP string: %s", ip_str);
        return NULL;
    }
    if (!parse_ip_string(gw_str, &ip_info.gw)) {
        ESP_LOGE(TAG, "Invalid gateway string: %s", gw_str);
        return NULL;
    }
    if (!parse_ip_string(netmask_str, &ip_info.netmask)) {
        ESP_LOGE(TAG, "Invalid netmask string: %s", netmask_str);
        return NULL;
    }

    esp_netif_dhcpc_stop(eth_netif);
    esp_netif_set_ip_info(eth_netif, &ip_info);

    spi_bus_config_t buscfg = {
        .miso_io_num = pins->pin_miso,
        .mosi_io_num = pins->pin_mosi,
        .sclk_io_num = pins->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(pins->spi_host, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 1 * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = pins->pin_cs,
    };

     eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(pins->spi_host, &spi_devcfg);

    w5500_config.int_gpio_num = pins->pin_int;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = pins->pin_rst;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    // The W5500 has no factory MAC, so its address register (SHAR)
    // is all zeros until we write one. Driver function pointer:
    // it writes our 6 bytes into the chip. It has to happen BEFORE
    // esp_eth_driver_install(), otherwise the driver starts with the
    // zero MAC and the netif copies that.
    ESP_ERROR_CHECK(mac->set_addr(mac, (uint8_t *)mac_addr));

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    //promiscuous mode.
    // Normally the W5500 drops every unicast frame whose destination MAC
    // does not match the address stored in its own hardware register.
    // With promiscuous ON the chip passes ALL received frames up to the
    // firmware, so its address filter is effectively bypassed.
    bool promiscuous = true;                     // true = accept all frames

    // esp_eth_ioctl talks to the driver; ETH_CMD_S_PROMISCUOUS is the
    // "set promiscuous mode" command and takes a pointer to a bool.
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous));

    // Read back the MAC the chip actually holds, so the log shows the
    // register value and not just what the netif believes.
    uint8_t chip_mac[6];
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, chip_mac));
    ESP_LOGI(TAG, "W5500 MAC register: %02x:%02x:%02x:%02x:%02x:%02x",
             chip_mac[0], chip_mac[1], chip_mac[2],
             chip_mac[3], chip_mac[4], chip_mac[5]);


    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return eth_netif;
}

esp_netif_t *ethernet_module1(const char *ip_str, const char *gw_str, const char *netmask_str)
{
    // Module 1 uses its own MAC ending in 01:01
    esp_netif_t *eth_netif = eth_init(ip_str, gw_str, netmask_str, &ETH1_PINS, ETH1_MAC);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return eth_netif;
}

esp_netif_t *ethernet_module2(const char *ip_str, const char *gw_str, const char *netmask_str)
{
    // Module 2 uses a different MAC ending in 02:01
    esp_netif_t *eth_netif = eth_init(ip_str, gw_str, netmask_str, &ETH2_PINS, ETH2_MAC);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return eth_netif;
}

static void sntp_retry_task(void *arg)
{
    sntp_time_init();
    vTaskDelete(NULL);
}

static void sntp_time_init(void)
{
    xSemaphoreTake(s_sntp_mutex, portMAX_DELAY);
    const char *plant_ntp_server = "10.17.19.30";

    setenv("TZ", "IST-5:30", 1);
    tzset();

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, plant_ntp_server);
    esp_sntp_init();

     ESP_LOGI(TAG, "Waiting for SNTP sync from %s...", plant_ntp_server);

    uint32_t start_ms = esp_log_timestamp();
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        if ((esp_log_timestamp() - start_ms) > 15000) {

            ESP_LOGW(TAG, "SNTP sync with %s timed out, giving up for now",
                     plant_ntp_server);
            xSemaphoreGive(s_sntp_mutex);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    xSemaphoreGive(s_sntp_mutex);
}

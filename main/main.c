#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define TAG "WiFiScanner"
#define MAX_APS 10
#define MAX_CLIENTS 10
#define SNIFF_TIME_MS 3000

#define GPS_UART_NUM UART_NUM_1
#define GPS_RXD 24
#define PPS_GPIO 23

static char gps_sentence[128] = {0};
static float last_lat = 0.0, last_lon = 0.0;
static bool gps_available = false;
static int64_t last_pps_time_us = 0;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int rssi;
    char ssid[33];
    int client_count;
    uint8_t clients[MAX_CLIENTS][6];
    int client_index;
} ap_info_t;

static ap_info_t ap_list[MAX_APS];
static int ap_list_count = 0;
static bool sniffing = false;

static void IRAM_ATTR pps_isr_handler(void* arg) {
    last_pps_time_us = esp_timer_get_time();
}

static void init_pps_gpio(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PPS_GPIO,
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PPS_GPIO, pps_isr_handler, NULL);
    ESP_LOGI(TAG, "PPS interrupt configured on GPIO%d", PPS_GPIO);
}

static void init_gps_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(GPS_UART_NUM, 2048, 0, 0, NULL, 0);
    uart_param_config(GPS_UART_NUM, &uart_config);
    uart_set_pin(GPS_UART_NUM, UART_PIN_NO_CHANGE, GPS_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "GPS UART initialized on RX=%d", GPS_RXD);
}

static bool detect_gps_presence(void) {
    char buffer[128];
    for (int i = 0; i < 30; i++) {
        int len = uart_read_bytes(GPS_UART_NUM, (uint8_t *)buffer, sizeof(buffer)-1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            buffer[len] = '\0';
            if (strstr(buffer, "$GP")) {
                ESP_LOGI(TAG, "GPS module detected");
                return true;
            }
        }
    }
    ESP_LOGW(TAG, "No GPS data detected - disabling GPS support");
    return false;
}

static void parse_gpgga(const char *nmea) {
    char buf[128];
    strncpy(buf, nmea, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *tokens[15] = {0};
    char *p = strtok(buf, ",");
    int idx = 0;
    while (p && idx < 15) {
        tokens[idx++] = p;
        p = strtok(NULL, ",");
    }

    if (idx < 6 || tokens[2] == NULL || tokens[4] == NULL) return;

    float lat = atof(tokens[2]);
    float lon = atof(tokens[4]);
    int lat_deg = (int)(lat / 100);
    float lat_min = lat - (lat_deg * 100);
    int lon_deg = (int)(lon / 100);
    float lon_min = lon - (lon_deg * 100);

    last_lat = lat_deg + (lat_min / 60.0);
    if (tokens[3][0] == 'S') last_lat *= -1;

    last_lon = lon_deg + (lon_min / 60.0);
    if (tokens[5][0] == 'W') last_lon *= -1;
}

static void poll_gps_data(void) {
    if (!gps_available) return;
    int len = uart_read_bytes(GPS_UART_NUM, (uint8_t *)gps_sentence, sizeof(gps_sentence)-1, 0);
    if (len > 0) {
        gps_sentence[len] = '\0';
        if (strstr(gps_sentence, "$GPGGA")) {
            parse_gpgga(gps_sentence);
        }
    }
}

static bool mac_in_list(uint8_t clients[][6], int count, const uint8_t *mac) {
    for (int i = 0; i < count; i++) {
        if (memcmp(clients[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void print_scan_header(void) {
    printf("\n| %-17s | %-4s | %-5s | %-6s | %-17s | %-12s | %-12s | %-10s |\n",
           "SSID", "Band", "Chan", "Clients", "BSSID", "Latitude", "Longitude", "PPS [ms]");
    printf("|-------------------|------|-------|--------|-------------------|--------------|--------------|------------|\n");
}

static void print_scan_entry(const ap_info_t *ap) {
    const char *band = "???";
    if (ap->channel >= 1 && ap->channel <= 14) band = "2.4G";
    else if (ap->channel >= 36 && ap->channel <= 165) band = "5G";
    else if (ap->channel >= 1 && ap->channel <= 233) band = "6G";

    char lat_buf[16], lon_buf[16], pps_buf[16];
    snprintf(lat_buf, sizeof(lat_buf), gps_available ? "%.5f" : "GPS: N/A", last_lat);
    snprintf(lon_buf, sizeof(lon_buf), gps_available ? "%.5f" : "GPS: N/A", last_lon);
    snprintf(pps_buf, sizeof(pps_buf), gps_available ? "%lld" : "N/A", last_pps_time_us / 1000);

    printf("| %-17s | %-4s | %-5d | %-6d | %02X:%02X:%02X:%02X:%02X:%02X | %-12s | %-12s | %-10s |\n",
           ap->ssid, band, ap->channel, ap->client_count,
           ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
           lat_buf, lon_buf, pps_buf);
}

void wifi_scan_task(void *pvParameters) {
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    while (1) {
        poll_gps_data();

        esp_wifi_set_promiscuous(false);
        esp_wifi_scan_start(&scan_cfg, true);

        uint16_t count = MAX_APS;
        wifi_ap_record_t results[MAX_APS] = {0};
        if (esp_wifi_scan_get_ap_records(&count, results) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get scan results");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        ap_list_count = count < MAX_APS ? count : MAX_APS;
        for (int i = 0; i < ap_list_count; i++) {
            memcpy(ap_list[i].bssid, results[i].bssid, 6);
            ap_list[i].channel = results[i].primary;
            ap_list[i].rssi = results[i].rssi;
            strncpy(ap_list[i].ssid, (char*)results[i].ssid, sizeof(ap_list[i].ssid) - 1);
            ap_list[i].ssid[32] = '\0';
            ap_list[i].client_index = 0;
        }

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(NULL);

        for (int i = 0; i < ap_list_count; i++) {
            sniffing = false;
            esp_wifi_set_channel(ap_list[i].channel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(100));
            sniffing = true;
            vTaskDelay(pdMS_TO_TICKS(SNIFF_TIME_MS));
            sniffing = false;
            ap_list[i].client_count = ap_list[i].client_index;
        }

        esp_wifi_set_promiscuous(false);

        print_scan_header();
        for (int i = 0; i < ap_list_count; i++) {
            print_scan_entry(&ap_list[i]);
        }

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    init_gps_uart();
    init_pps_gpio();
    gps_available = detect_gps_presence();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 8192, NULL, 5, NULL);
}

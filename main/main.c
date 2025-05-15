#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
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
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "sdkconfig.h"
#include "OLEDDisplay.h"
#include "driver/i2c.h"

#define TAG "WiFiScanner"
#define MAX_APS 10
#define MAX_CLIENTS 10
#define SNIFF_TIME_MS 3000
#define GPS_UART_NUM UART_NUM_1
#define GPS_RXD 24
#define PPS_GPIO 23
#define RETAIN_CLIENTS_HISTORY 1 // set to 0 to clear client list each scan
#define SORT_RESULTS_BY_RSSI 1   // set to 0 to disable RSSI-based sorting

#define _I2C_NUMBER(num) I2C_NUM_0
#define I2C_NUMBER(num) _I2C_NUMBER(num)
#define I2C_MASTER_SCL_IO 5									  /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO 4									  /*!< gpio number for I2C master data  */
#define I2C_MASTER_NUM I2C_NUMBER(CONFIG_I2C_MASTER_PORT_NUM) /*!< I2C port number for master dev */

SemaphoreHandle_t print_mux = NULL;

static char gps_sentence[128] = {0};
static float last_lat = 0.0, last_lon = 0.0;
static bool gps_available = false;
static int64_t last_pps_time_us = 0;

typedef struct {
    char ssid[33];
    uint8_t channel;
    int rssi;
    uint8_t bssid[6];
    int client_count;
    wifi_auth_mode_t authmode;
} scan_result_t;

static scan_result_t ap_results[MAX_APS];
static int ap_result_count = 0;
static uint8_t client_macs[MAX_APS][MAX_CLIENTS][6];
static int client_counts[MAX_APS] = {0};

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

static bool mac_in_list(uint8_t (*list)[6], int count, const uint8_t *mac) {
    for (int i = 0; i < count; i++) {
        if (memcmp(list[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void wifi_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;

    const uint8_t *bssid = payload + 10;
    const uint8_t *src = payload + 16;

    for (int i = 0; i < ap_result_count; i++) {
        if (memcmp(ap_results[i].bssid, bssid, 6) == 0) {
            if (!mac_in_list(client_macs[i], client_counts[i], src) && client_counts[i] < MAX_CLIENTS) {
                memcpy(client_macs[i][client_counts[i]], src, 6);
                client_counts[i]++;
                ap_results[i].client_count = client_counts[i];
            }
            break;
        }
    }
}




static int compare_rssi(const void *a, const void *b) {
    const scan_result_t *ra = (const scan_result_t *)a;
    const scan_result_t *rb = (const scan_result_t *)b;
    return rb->rssi - ra->rssi;
}

static void print_scan_results(void) {
    if (SORT_RESULTS_BY_RSSI && ap_result_count > 1) {
        qsort(ap_results, ap_result_count, sizeof(scan_result_t), compare_rssi);
    }

    printf("\n| %-25s | %-4s | %-5s | %-6s | %-4s | %-17s | %-10s | %-12s | %-12s | %-10s |\n",
           "SSID", "Band", "Chan", "RSSI", "Cli", "BSSID", "Security", "Latitude", "Longitude", "PPS [ms]");
    printf("|---------------------------|------|-------|--------|------|-------------------|------------|--------------|--------------|------------|\n");

    for (int i = 0; i < ap_result_count; i++) {
        const char *band = (ap_results[i].channel <= 14) ? "2.4G" : "5G";

        char lat_buf[16], lon_buf[16], pps_buf[16];
        snprintf(lat_buf, sizeof(lat_buf), gps_available ? "%.5f" : "GPS: N/A", last_lat);
        snprintf(lon_buf, sizeof(lon_buf), gps_available ? "%.5f" : "GPS: N/A", last_lon);
        snprintf(pps_buf, sizeof(pps_buf), gps_available ? "%lld" : "N/A", last_pps_time_us / 1000);

        const char *auth_mode = "OPEN";
        switch (ap_results[i].authmode) {
            case WIFI_AUTH_WEP: auth_mode = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_mode = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_mode = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_mode = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK: auth_mode = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_mode = "WPA2/WPA3"; break;
            default: break;
        }

        printf("| %-25s | %-4s | %-5d | %-6d | %-4d | %02X:%02X:%02X:%02X:%02X:%02X | %-10s | %-12s | %-12s | %-10s |\n",
               ap_results[i].ssid, band, ap_results[i].channel, ap_results[i].rssi, ap_results[i].client_count,
               ap_results[i].bssid[0], ap_results[i].bssid[1], ap_results[i].bssid[2],
               ap_results[i].bssid[3], ap_results[i].bssid[4], ap_results[i].bssid[5],
               auth_mode, lat_buf, lon_buf, pps_buf);
    }
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

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "Free heap before scan: %d bytes", free_heap);

        esp_wifi_set_promiscuous(false);
        esp_wifi_scan_start(&scan_cfg, true);

        wifi_ap_record_t results[MAX_APS] = {0};
        uint16_t count = MAX_APS;
        if (esp_wifi_scan_get_ap_records(&count, results) == ESP_OK) {
            ap_result_count = 0;
            for (int i = 0; i < count && ap_result_count < MAX_APS; i++) {
                scan_result_t *entry = &ap_results[ap_result_count];
                strncpy(entry->ssid, (char *)results[i].ssid, sizeof(entry->ssid) - 1);
                entry->ssid[32] = '\0';
                entry->channel = results[i].primary;
                entry->rssi = results[i].rssi;
                memcpy(entry->bssid, results[i].bssid, 6);
                entry->authmode = results[i].authmode;
                if (!RETAIN_CLIENTS_HISTORY) {
                    memset(client_macs[ap_result_count], 0, sizeof(client_macs[ap_result_count]));
                    client_counts[ap_result_count] = 0;
                }
                entry->client_count = client_counts[ap_result_count];
                ap_result_count++;
            }
        }

        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_callback);
        esp_wifi_set_promiscuous(true);

        for (int i = 0; i < ap_result_count; i++) {
            esp_wifi_set_channel(ap_results[i].channel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(SNIFF_TIME_MS));
        }

        esp_wifi_set_promiscuous(false);
        print_scan_results();

        free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "Free heap after scan: %d bytes", free_heap);

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}


static void i2c_small_text_list(void *arg)
{
	OLEDDisplay_t *oled = OLEDDisplay_init(I2C_MASTER_NUM, 0x78, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
	OLEDDisplay_flipScreenVertically(oled);
	OLEDDisplay_setTextAlignment(oled, TEXT_ALIGN_LEFT);
	OLEDDisplay_setFont(oled, ArialMT_Plain_10);

	int page = 0;			 
	int networks_per_page = 4; 

	while (1)
	{
		OLEDDisplay_clear(oled);         
        OLEDDisplay_drawString(oled, 0, 00, "Networks");

		for (int i = 0; i < networks_per_page; i++)
		{
			int index = page * networks_per_page + i;
			if (index < ap_result_count)
			{
				OLEDDisplay_drawString(oled, 0, 15 + (i * 10), ap_results[index].ssid);
			}
		}

		OLEDDisplay_display(oled);			  
		vTaskDelay(8000 / portTICK_PERIOD_MS); 

		page++;
		if (page * networks_per_page >= ap_result_count)
		{
			page = 0; 
		}
	}
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    init_gps_uart();
    init_pps_gpio();
    gps_available = detect_gps_presence();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 8192, NULL, 5, NULL);

    print_mux = xSemaphoreCreateMutex();
	xTaskCreate(i2c_small_text_list, "i2c_test_task_0", 8192, (void *)0, 10, NULL);
}

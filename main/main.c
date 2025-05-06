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

#define TAG "WiFiScanner"
#define MAX_APS 10
#define MAX_CLIENTS 10
#define SNIFF_TIME_MS 3000

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

static bool mac_in_list(uint8_t clients[][6], int count, const uint8_t *mac) {
    for (int i = 0; i < count; i++) {
        if (memcmp(clients[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void wifi_sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!sniffing) return;
    const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = ppkt->payload;

    if ((payload[0] & 0x0C) == 0x08) { // data frame
        const uint8_t *src_mac = payload + 10;
        const uint8_t *bssid = payload + 16;

        for (int i = 0; i < ap_list_count; i++) {
            if (memcmp(ap_list[i].bssid, bssid, 6) == 0) {
                if (!mac_in_list(ap_list[i].clients, ap_list[i].client_index, src_mac)) {
                    if (ap_list[i].client_index < MAX_CLIENTS) {
                        memcpy(ap_list[i].clients[ap_list[i].client_index], src_mac, 6);
                        ap_list[i].client_index++;
                    }
                }
            }
        }
    }
}

void wifi_scan_task(void *pvParameters) {
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true
    };

    while (1) {
        ESP_LOGI(TAG, "Starting scan...");
        esp_wifi_set_promiscuous(false);
        esp_wifi_scan_start(&scan_config, true);

        uint16_t count = MAX_APS;
        wifi_ap_record_t scan_results[MAX_APS] = {0};
        if (esp_wifi_scan_get_ap_records(&count, scan_results) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get AP records");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        ap_list_count = count < MAX_APS ? count : MAX_APS;
        for (int i = 0; i < ap_list_count; i++) {
            memcpy(ap_list[i].bssid, scan_results[i].bssid, 6);
            ap_list[i].channel = scan_results[i].primary;
            ap_list[i].rssi = scan_results[i].rssi;
            memcpy(ap_list[i].ssid, scan_results[i].ssid, sizeof(ap_list[i].ssid));
            ap_list[i].ssid[32] = '\0';
            ap_list[i].client_index = 0;
        }

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_callback);

        // Sniff each AP for a configurable duration
        for (int i = 0; i < ap_list_count; i++) {
            sniffing = false;
            esp_wifi_set_channel(ap_list[i].channel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(100)); // settle on channel
            sniffing = true;
            vTaskDelay(pdMS_TO_TICKS(SNIFF_TIME_MS));
            sniffing = false;
            ap_list[i].client_count = ap_list[i].client_index;
        }

        esp_wifi_set_promiscuous(false);

        printf("\n| %-17s | %-4s | %-5s | %-6s | %-17s |\n", "SSID", "Band", "Chan", "Clients", "BSSID");
        printf("|-------------------|------|-------|--------|-------------------|\n");

        for (int i = 0; i < ap_list_count; i++) {
            const char *band = "???";
            uint8_t ch = ap_list[i].channel;

            if (ch >= 1 && ch <= 14) {
                band = "2.4G";
            } else if (ch >= 36 && ch <= 165) {
                band = "5G";
            } else if (ch >= 1 && ch <= 233) {
                band = "6G"; // if supported
            }

            printf("| %-17s | %-4s | %-5d | %-6d | %02X:%02X:%02X:%02X:%02X:%02X |\n",
                   ap_list[i].ssid,
                   band,
                   ch,
                   ap_list[i].client_count,
                   ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                   ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
        }

        vTaskDelay(pdMS_TO_TICKS(60000)); // wait 60 sec before next full cycle
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 8192, NULL, 5, NULL);
}
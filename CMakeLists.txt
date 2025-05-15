cmake_minimum_required(VERSION 3.22)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Only build these components (and whatever they themselves require)
set(COMPONENTS
    main           # your app
    freertos       # FreeRTOS kernel
    esp_timer      # for esp_timer_get_time()
    esp_log        # logging
    esp_netif      # network-interface init
    esp_event      # event loop
    esp_wifi       # Wi-Fi driver
    esp_mac        # MAC address APIs
    nvs_flash      # NVS for default configs
    vfs            # virtual filesystem abstraction
    fatfs          # FAT filesystem layer
    sdspi          # SDSPI host driver
    spi_common     # SPI bus driver support
    sdmmc          # SD/MMC support (if you want SD/MMC mode too)
    driver         # core driver umbrella (needed by spi_common & sdspi)
    oled           # oled screen
)

# Now bring in the normal IDF machinery
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_scan)

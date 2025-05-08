idf_component_register(
    SRCS 
        "main.c"
        "st7735s.c"
        "fontx.c"
    INCLUDE_DIRS 
        "."
    PRIV_REQUIRES 
        esp_system
        esp_hw_support
        nvs_flash
        spiffs
        driver
        esp_wifi
        esp_timer
)

spiffs_create_partition_image(spiffs ../spiffs_image FLASH_IN_PROJECT)

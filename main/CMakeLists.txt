idf_component_register(SRCS "main.c" "watch.c" "clock.c" "tamagotchi.c" "dolphin.c" "ad_monitor.c"
                    INCLUDE_DIRS "."
                    REQUIRES "esp_bsp" "lvgl__lvgl" "espressif__esp_lvgl_port" "nvs_flash" "driver" "esp_wifi" "esp_netif" "esp_adc")

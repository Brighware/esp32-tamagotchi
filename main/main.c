/*
 * Dolphin Tamagotchi  —  Waveshare ESP32-C6-Touch-LCD-1.47
 *
 * Hardware (verified from Waveshare BSP):
 *   LCD  JD9853  SPI2  MOSI=2 SCLK=1 MISO=3  CS=14 DC=15 RST=22 BL=23
 *        172x320, color inverted, column gap 34 (rotation 0), 80 MHz
 *   Touch AXS5106L  I2C0 (addr 0x63)  SDA=18 SCL=19 INT=21 RST=20
 *
 * main.c brings up SPI + panel + touch + LVGL, then hands control to the game
 * in tamagotchi.c. Game logic and drawing run inside LVGL timers / input
 * callbacks (LVGL task), so they are single threaded and need no extra locking.
 */
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"

#include "esp_lvgl_port.h"

#include "bsp_display.h"
#include "bsp_touch.h"
#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "wifi.h"
#include "watch.h"
#include "sntp.h"

#define EXAMPLE_DISPLAY_ROTATION 0
#define EXAMPLE_LCD_H_RES (172)
#define EXAMPLE_LCD_V_RES (320)
#define EXAMPLE_LCD_DRAW_BUFF_HEIGHT (50)
#define EXAMPLE_LCD_DRAW_BUFF_DOUBLE (1)

static const char *TAG = "tamagotchi_main";

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

static esp_err_t app_lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 1024 * 10,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT,
        .double_buffer = EXAMPLE_LCD_DRAW_BUFF_DOUBLE,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        },
    };
    /* 172-wide panel sits at column offset 34 on the JD9853 (rotation 0) */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 34, 0));
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    if (touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lvgl_disp,
            .handle = touch_handle,
        };
        lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    }

    return ESP_OK;
}

void app_main(void)
{
    /* NVS holds the persisted pet between power cycles */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_init();
    bsp_spi_init();
    bsp_display_init(&io_handle, &panel_handle, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t));

    esp_err_t tret = bsp_touch_init(&touch_handle, i2c_bus, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, EXAMPLE_DISPLAY_ROTATION);
    if (tret != ESP_OK) {
        touch_handle = NULL;   /* run without touch rather than crash-loop */
        ESP_LOGW(TAG, "touch unavailable (%s) — buttons will not respond", esp_err_to_name(tret));
    }

    ESP_ERROR_CHECK(app_lvgl_init());
    wifi_start();
    sntp_start();
    bsp_display_brightness_init();
    bsp_display_set_brightness(70);
    ESP_LOGI(TAG, "Starting TamaWatchy watch shell");
    if (lvgl_port_lock(0)) {
        watch_start();
        lvgl_port_unlock();
    }
}

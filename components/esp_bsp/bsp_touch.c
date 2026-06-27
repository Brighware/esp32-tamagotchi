#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "esp_lcd_touch_axs5106.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_touch.h"

static const char *TP_TAG = "bsp_touch";

esp_err_t bsp_touch_init(esp_lcd_touch_handle_t *touch_handle, i2c_master_bus_handle_t bus_handle, uint16_t xmax, uint16_t ymax, uint16_t rotation)
{
    static i2c_master_dev_handle_t dev_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESP_LCD_TOUCH_IO_I2C_AXS5106_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TP_TAG, "i2c add device failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = xmax < ymax ? xmax : ymax;
    tp_cfg.y_max = xmax < ymax ? ymax : xmax;
    tp_cfg.rst_gpio_num = EXAMPLE_PIN_TP_RST;
    tp_cfg.int_gpio_num = EXAMPLE_PIN_TP_INT;

    if (90 == rotation) {
        tp_cfg.flags.swap_xy = 1;
        tp_cfg.flags.mirror_x = 0;
        tp_cfg.flags.mirror_y = 0;
    } else if (180 == rotation) {
        tp_cfg.flags.swap_xy = 0;
        tp_cfg.flags.mirror_x = 0;
        tp_cfg.flags.mirror_y = 1;
    } else if (270 == rotation) {
        tp_cfg.flags.swap_xy = 1;
        tp_cfg.flags.mirror_x = 1;
        tp_cfg.flags.mirror_y = 1;
    } else {
        tp_cfg.flags.swap_xy = 0;
        tp_cfg.flags.mirror_x = 1;
        tp_cfg.flags.mirror_y = 0;
    }

    /* Retry instead of aborting the firmware if the controller is slow to wake */
    for (int attempt = 1; attempt <= 6; attempt++) {
        err = esp_lcd_touch_new_i2c_axs5106(dev_handle, &tp_cfg, touch_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TP_TAG, "AXS5106 ready (attempt %d)", attempt);
            return ESP_OK;
        }
        ESP_LOGW(TP_TAG, "AXS5106 init attempt %d failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    ESP_LOGE(TP_TAG, "AXS5106 not responding; continuing without touch");
    return err;
}

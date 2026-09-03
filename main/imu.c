#include "imu.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include <stdio.h>
#include <stdbool.h>

static i2c_master_bus_handle_t imu_bus_handle;

static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true
    };
    return i2c_new_master_bus(&bus_config, bus_handle);
}


void init_imu(void)
{
  imu_bus_handle = NULL;
  esp_err_t ret = qmi8658_init(&dev, imu_bus_handle, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMI8658 (error: %d)", ret);
        vTaskDelete(NULL);
    }
}


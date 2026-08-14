#include "ad_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/** NOTES:
* BAT_ADC = GPIO0
* GPIO0 Channel = ADC1_CH0 = ADC_CHANNEL_2
*/

#define AD_MONITOR_CHANNEL     ADC_CHANNEL_2
#define EXAMPLE_ADC1_CHAN0     ADC_CHANNEL_2
#define AD_MONITOR_ATTEN       ADC_ATTEN_DB_12

static const char *AD_TAG = "ad_battery_monitor";

static int adc_raw;
static int voltage;
static bool ad_monitor_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
static void ad_monitor_calibration_deinit(adc_cali_handle_t handle);
static adc_oneshot_unit_handle_t adc1_handle;

int get_raw_ad_value(void)
{
  return(adc_raw);
}

int get_ad_voltage(void)
{
  return(voltage);
}

void init_ad_monitor(void)
{
    //-------------ADC1 Init---------------//
    adc1_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .atten = AD_MONITOR_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, AD_MONITOR_CHANNEL, &config));


}

void exec_ad_monitor(void)
{
    //-------------ADC1 Calibration Init---------------//
    adc_cali_handle_t adc1_cali_chan0_handle = NULL;
    bool do_calibration1_chan0 = ad_monitor_calibration_init(ADC_UNIT_1, AD_MONITOR_CHANNEL, AD_MONITOR_ATTEN, &adc1_cali_chan0_handle);

    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, AD_MONITOR_CHANNEL, &adc_raw));
    ESP_LOGI(AD_TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, AD_MONITOR_CHANNEL, adc_raw);
    if (do_calibration1_chan0) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle, adc_raw, &voltage));
        ESP_LOGI(AD_TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, AD_MONITOR_CHANNEL, voltage);
    }
//    vTaskDelay(pdMS_TO_TICKS(1000));

}

/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
static bool ad_monitor_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(AD_TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(AD_TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(AD_TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(AD_TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(AD_TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

static void ad_monitor_calibration_deinit(adc_cali_handle_t handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGI(AD_TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGI(AD_TAG, "deregister %s calibration scheme", "Line Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));
#endif
}

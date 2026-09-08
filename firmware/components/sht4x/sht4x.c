/* sht4x.c — ESP-IDF I2C driver for the Sensirion SHT4x (shared I2C bus). */
#include "sht4x.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#define TAG "sht4x"
#define CMD_MEASURE_HIGH 0xFD   /* measure T & RH, high precision (~8.2 ms) */
#define CMD_SOFT_RESET   0x94

static struct {
    i2c_master_dev_handle_t dev;
    bool ready;
} s;

int sht4x_init(void *i2c_bus, uint8_t addr)
{
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_bus, &dcfg, &s.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device @0x%02x: %s", addr, esp_err_to_name(err));
        return err;
    }

    uint8_t cmd = CMD_SOFT_RESET;
    i2c_master_transmit(s.dev, &cmd, 1, 100);
    vTaskDelay(pdMS_TO_TICKS(2));                 /* soft-reset time ~1 ms */
    s.ready = true;
    ESP_LOGI(TAG, "SHT4x @0x%02x ready", addr);
    return ESP_OK;
}

int sht4x_read(float *temp_c, float *rh_pct, bool *fault)
{
    if (fault) *fault = false;
    if (!s.ready) { if (fault) *fault = true; return ESP_FAIL; }

    uint8_t cmd = CMD_MEASURE_HIGH;
    if (i2c_master_transmit(s.dev, &cmd, 1, 100) != ESP_OK) {
        if (fault) *fault = true; return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));                /* wait for conversion */

    uint8_t buf[6];
    if (i2c_master_receive(s.dev, buf, sizeof(buf), 100) != ESP_OK) {
        if (fault) *fault = true; return ESP_FAIL;
    }
    /* Validate both CRCs (T triplet and RH triplet). */
    if (sht4x_crc8(&buf[0], 2) != buf[2] || sht4x_crc8(&buf[3], 2) != buf[5]) {
        if (fault) *fault = true; return ESP_FAIL;
    }

    uint16_t t_ticks  = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t rh_ticks = ((uint16_t)buf[3] << 8) | buf[4];
    if (temp_c) *temp_c = sht4x_ticks_to_c(t_ticks);
    if (rh_pct) *rh_pct = sht4x_ticks_to_rh(rh_ticks);
    return ESP_OK;
}

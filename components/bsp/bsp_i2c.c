#include "bsp.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "bsp_i2c";
static i2c_master_bus_handle_t s_bus;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_GPIO,
        .scl_io_num = BSP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I2C ready: SDA=IO%d SCL=IO%d %dHz", BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return s_bus;
}

esp_err_t bsp_i2c_add_device(uint8_t addr, uint32_t speed_hz, i2c_master_dev_handle_t *out_dev)
{
    if (s_bus == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = speed_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
    return i2c_master_bus_add_device(s_bus, &dev, out_dev);
}

esp_err_t bsp_i2c_scan(uint8_t *addrs, size_t max, size_t *found)
{
    if (s_bus == NULL || found == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "found device 0x%02X", a);
            if (addrs && *found < max) {
                addrs[*found] = a;
            }
            (*found)++;
        }
    }
    return ESP_OK;
}

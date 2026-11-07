#include "lcd_touch.h"

#include "bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_RST 39
#define PIN_INT 55
#define REG_PRODUCT_ID 0x8140
#define REG_STATUS     0x814E
#define REG_POINT      0x8150

static const char *TAG = "lcd_touch";
static i2c_master_dev_handle_t s_dev;

static esp_err_t read_reg(uint16_t reg, uint8_t *out, size_t len)
{
    uint8_t addr[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, addr, 2, out, len, 100);
}

static esp_err_t write_reg(uint16_t reg, uint8_t value)
{
    uint8_t packet[3] = {reg >> 8, reg & 0xFF, value};
    return i2c_master_transmit(s_dev, packet, sizeof(packet), 100);
}

esp_err_t lcd_touch_init(void)
{
    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&reset_cfg);
    if (err != ESP_OK) return err;
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&int_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_pullup_dis(PIN_INT);
    vTaskDelay(pdMS_TO_TICKS(120));
    err = bsp_i2c_add_device(0x14, 100000, &s_dev);
    if (err != ESP_OK) return err;
    uint8_t product[5] = {0};
    err = read_reg(REG_PRODUCT_ID, product, 4);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT1151Q not responding: %s", esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "touch controller: %.4s", (char *)product);
    return ESP_OK;
}

lcd_touch_event_t lcd_touch_poll(uint16_t *x, uint16_t *y)
{
    if (!s_dev || !x || !y) return LCD_TOUCH_NONE;
    uint8_t status = 0;
    if (read_reg(REG_STATUS, &status, 1) != ESP_OK || !(status & 0x80))
        return LCD_TOUCH_NONE;
    uint8_t count = status & 0x0F;
    if (count == 0) {
        write_reg(REG_STATUS, 0);
        return LCD_TOUCH_RELEASE;
    }
    if (count > 5) {
        write_reg(REG_STATUS, 0);
        return LCD_TOUCH_NONE;
    }
    uint8_t point[4] = {0};
    bool valid = read_reg(REG_POINT, point, sizeof(point)) == ESP_OK;
    write_reg(REG_STATUS, 0);
    if (!valid) return LCD_TOUCH_NONE;
    const uint16_t raw_x = point[0] | (point[1] << 8);
    const uint16_t raw_y = point[2] | (point[3] << 8);
    /* Board example maps landscape x = width - raw_y, y = raw_x. */
    if (raw_x >= 320 || raw_y >= 480) return LCD_TOUCH_NONE;
    *x = 479 - raw_y;
    *y = raw_x;
    return LCD_TOUCH_POINT;
}

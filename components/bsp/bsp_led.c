#include "bsp.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "bsp_led";

/* 板载 RGB 灯：WS2812 (XL-2121RGBC-2812B)，接模组 IO37 */
#define BSP_LED_GPIO 37

static led_strip_handle_t s_strip;

esp_err_t bsp_led_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "WS2812 ready on IO%d", BSP_LED_GPIO);
    return ESP_OK;
}

esp_err_t bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = led_strip_set_pixel(s_strip, 0, r, g, b);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    return err;
}

esp_err_t bsp_led_off(void)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_strip);
}

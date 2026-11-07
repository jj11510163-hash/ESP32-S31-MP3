#include "app_core.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "input";

/* 板载 BOOT 键：模组 IO61，按下为低电平 */
#define INPUT_BOOT_GPIO GPIO_NUM_61
#define INPUT_POLL_MS 10
#define INPUT_DEBOUNCE_MS 40
#define INPUT_MULTI_CLICK_MS 330
#define INPUT_LONG_PRESS_MS 900

static app_input_handler_t s_handler;
static TaskHandle_t s_task;

static const char *const k_key_names[APP_KEY_MAX] = {
    [APP_KEY_PREV] = "PREV",     [APP_KEY_NEXT] = "NEXT",   [APP_KEY_PLAY] = "PLAY",
    [APP_KEY_ENTER] = "ENTER",   [APP_KEY_BACK] = "BACK",   [APP_KEY_MENU] = "MENU",
    [APP_KEY_ROT_CW] = "ROT_CW", [APP_KEY_ROT_CCW] = "ROT_CCW",
    [APP_KEY_UP] = "UP",         [APP_KEY_DOWN] = "DOWN",
    [APP_KEY_KEY0_SINGLE] = "KEY0_SINGLE", [APP_KEY_KEY0_DOUBLE] = "KEY0_DOUBLE",
    [APP_KEY_KEY0_TRIPLE] = "KEY0_TRIPLE", [APP_KEY_KEY0_LONG] = "KEY0_LONG",
    [APP_KEY_PAGE_PREV] = "PAGE_PREV", [APP_KEY_PAGE_NEXT] = "PAGE_NEXT",
    [APP_KEY_KEY0_QUAD] = "KEY0_QUAD",
};

const char *app_key_name(app_key_t key)
{
    if (key < 0 || key >= APP_KEY_MAX) {
        return "?";
    }
    return k_key_names[key] ? k_key_names[key] : "?";
}

esp_err_t app_input_register_handler(app_input_handler_t handler)
{
    s_handler = handler;
    return ESP_OK;
}

void app_input_post(app_key_t key, int32_t value)
{
    if (key < 0 || key >= APP_KEY_MAX) {
        return;
    }
    if (s_handler && s_handler(key, value)) {
        return; /* 已被外部后端消费 */
    }
    app_evt_t evt = {0};
    evt.id = APP_EVT_INPUT;
    evt.input.key = (uint8_t)key;
    evt.input.value = value;
    app_evt_post(&evt, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    bool raw_low = false;
    bool stable_low = false;
    int64_t raw_changed_at = 0;
    int64_t pressed_at = 0;
    int64_t released_at = 0;
    uint8_t clicks = 0;
    bool long_sent = false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << INPUT_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "KEY0 on GPIO%d (single/double/triple/quad/long)", INPUT_BOOT_GPIO);

    for (;;) {
        bool level_low = gpio_get_level(INPUT_BOOT_GPIO) == 0;
        int64_t now = esp_timer_get_time() / 1000;

        if (level_low != raw_low) {
            raw_low = level_low;
            raw_changed_at = now;
        }
        if (stable_low != raw_low && now - raw_changed_at >= INPUT_DEBOUNCE_MS) {
            stable_low = raw_low;
            if (stable_low) {
                pressed_at = now;
                long_sent = false;
            } else if (!long_sent) {
                clicks++;
                released_at = now;
            }
        }
        if (stable_low && !long_sent && now - pressed_at >= INPUT_LONG_PRESS_MS) {
            long_sent = true;
            clicks = 0;
            ESP_LOGI(TAG, "KEY0 long");
            app_input_post(APP_KEY_KEY0_LONG, 1);
        }
        if (!stable_low && clicks &&
            (clicks >= 4 || now - released_at >= INPUT_MULTI_CLICK_MS)) {
            app_key_t key = clicks >= 4 ? APP_KEY_KEY0_QUAD :
                            clicks == 3 ? APP_KEY_KEY0_TRIPLE :
                            clicks == 2 ? APP_KEY_KEY0_DOUBLE : APP_KEY_KEY0_SINGLE;
            clicks = 0;
            ESP_LOGI(TAG, "%s", app_key_name(key));
            app_input_post(key, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
}

esp_err_t app_input_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(input_task, "input", 3072, NULL, 4, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

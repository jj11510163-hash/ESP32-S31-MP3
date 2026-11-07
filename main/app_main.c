/*
 * ESP32-S31 蓝牙音乐播放器
 *
 *   慧勤智远 ESP32-S31 CB V1.1 (WROOM-3 N16R16)
 *   SD 卡(SDIO 4-bit) -> MP3 解码 -> A2DP Source(SBC) -> 蓝牙耳机
 *   ST7796U 480x320 SPI LCD + GT1151Q 触摸：浅色播放首页
 *
 * 启动顺序：设置/事件总线 -> I2C/LED/按键 -> 曲库/播放器 -> UI(开机动画)
 *           -> 经典蓝牙 -> SD 挂载与扫描 -> 预留服务(SPP/WiFi AP)
 */
#include <stdio.h>

#include "app_core.h"
#include "bsp.h"
#include "bt_audio.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music.h"
#include "net_service.h"
#include "ui.h"

static const char *TAG = "app";

static void confirm_updated_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) ESP_LOGI(TAG, "updated app confirmed");
        else ESP_LOGE(TAG, "updated app confirmation failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/* RGB 状态灯                                                          */
/* ------------------------------------------------------------------ */
static void led_task(void *arg)
{
    (void)arg;
    bool on = false;
    for (;;) {
        bt_state_t st = bt_audio_state();
        uint8_t r = 0, g = 0, b = 0;
        switch (st) {
        case BT_ST_STREAMING:
            g = 12; /* 播放中：绿色呼吸 */
            break;
        case BT_ST_CONNECTED:
            b = 10; /* 已连接：蓝色 */
            break;
        case BT_ST_DISCOVERING:
        case BT_ST_CONNECTING:
            r = 12;
            g = 6; /* 搜索/连接：橙色 */
            break;
        case BT_ST_ERROR:
            r = 16; /* 错误：红色 */
            break;
        default:
            b = on ? 2 : 6; /* 待机：暗蓝闪 */
            break;
        }
        if (st == BT_ST_STREAMING) {
            g = on ? 6 : 14;
        }
        bsp_led_set(r, g, b);
        on = !on;
        vTaskDelay(pdMS_TO_TICKS(600));
    }
}

/* ------------------------------------------------------------------ */
/* SD 挂载（带重试，避免阻塞 UI 启动）                                 */
/* ------------------------------------------------------------------ */
static void sd_task(void *arg)
{
    (void)arg;
    int attempt = 0;
    for (;;) {
        if (bsp_sd_mount() == ESP_OK) {
            app_evt_t evt = {.id = APP_EVT_SD_STATE};
            evt.sd.mounted = true;
            app_evt_post(&evt, 0);
            music_lib_scan();
            vTaskDelete(NULL);
            return;
        }
        attempt++;
        if (attempt == 6) {
            ESP_LOGW(TAG, "SD not mounted yet; retrying every 5 seconds");
            app_evt_t evt = {.id = APP_EVT_SD_STATE};
            evt.sd.mounted = false;
            app_evt_post(&evt, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(attempt < 6 ? 1500 : 5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S31 Bluetooth Music Player  (IDF %s)", "6.1");
    ESP_LOGI(TAG, " board: ESP32-S31 CB V1.1 / WROOM-3 N16R16");
    ESP_LOGI(TAG, " firmware: %s %s  [lcd-home-v1]", __DATE__, __TIME__);
    ESP_LOGI(TAG, "==================================================");

    /* 1. 基础服务 */
    ESP_ERROR_CHECK(app_settings_init());
    ESP_ERROR_CHECK(app_events_init());

    /* 2. 板级外设 */
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "i2c init failed");
    }
    bsp_led_init();
    app_input_init();

    /* 3. 音频链路 */
    music_lib_init();
    ESP_ERROR_CHECK(player_init());

    /* 4. UI（尽早出画面） */
    ESP_ERROR_CHECK(ui_init());
    bool panel_ready = ui_has_panel();
    if (!panel_ready) {
        ESP_LOGW(TAG, "LCD initialization failed — check SPI screen wiring");
    }

    /* Start SD before the synchronous Bluetooth init so a stalled BT stack
     * cannot prevent card detection and library scanning. */
    if (xTaskCreate(sd_task, "sd", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start SD task");
    }

    /* 5. 经典蓝牙（耗时较长，放在 UI 起来之后） */
    bt_audio_set_codec_pref(app_settings_get()->bt_codec);
    esp_err_t bt_err = bt_audio_init();
    if (bt_err != ESP_OK) {
        ESP_LOGE(TAG, "bluetooth init failed — playback unavailable");
    }
    bt_app_link_init(); /* 预留：手机 APP 通道 */

    /* Confirm only after essential services work; card and earphones are optional. */
    if (panel_ready && bt_err == ESP_OK) confirm_updated_app();

    /* WiFi/AP is initialized when the user enables web transfer. */

    /* 7. 状态灯 */
    xTaskCreate(led_task, "led", 2560, NULL, 2, NULL);

    /* 最后再启动串口遥控：它即使失败也不影响 UI / 蓝牙 / SD */
    if (app_serial_input_start() != ESP_OK) {
        ESP_LOGW(TAG, "serial input unavailable");
    }

    ESP_LOGI(TAG, "boot done: heap=%u psram=%u dma=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

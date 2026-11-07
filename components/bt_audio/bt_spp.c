/*
 * bt_spp.c —�?预留：手�?APP 通道（经典蓝�?SPP�? *
 * 手机连上 SPP 后，可以用一个简单的行协议控制播放器 / 拉取状态�? * 协议格式（UTF-8，\n 结尾）：
 *   CMD PLAY | PAUSE | NEXT | PREV | VOL <0..127> | LIB | STATUS
 *   回复：OK ... / ERR ...
 */
#include "bt_audio.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "spp";

#define SPP_APP_ID 0x5A31
#define SPP_MAX_DATA 512

static uint32_t s_spp_handle;
static bool s_connected;
static bt_app_link_rx_cb_t s_rx_cb;
static QueueHandle_t s_txq;

typedef struct {
    uint32_t handle;
    uint16_t len;
    uint8_t data[SPP_MAX_DATA];
} spp_tx_msg_t;

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "S31CTRL");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        s_spp_handle = param->srv_open.handle;
        s_connected = true;
        ESP_LOGI(TAG, "phone app connected");
        break;
    case ESP_SPP_CLOSE_EVT:
        s_connected = false;
        ESP_LOGI(TAG, "phone app disconnected");
        break;
    case ESP_SPP_DATA_IND_EVT:
        if (param->data_ind.len > 0) {
            if (s_rx_cb) {
                s_rx_cb(param->data_ind.data, param->data_ind.len);
            }
            /* 默认回一�?ACK，方便手机端联调 */
            const char ack[] = "OK\n";
            esp_spp_write(param->data_ind.handle, sizeof(ack) - 1, (uint8_t *)ack);
        }
        break;
    default:
        break;
    }
}

esp_err_t bt_app_link_init(void)
{
    esp_spp_cfg_t cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,
        .tx_buffer_size = 0,
    };
    esp_err_t err = esp_spp_register_callback(spp_cb);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_spp_enhanced_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spp init failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool bt_app_link_connected(void)
{
    return s_connected;
}

void bt_app_link_set_rx_cb(bt_app_link_rx_cb_t cb)
{
    s_rx_cb = cb;
}

int bt_app_link_send(const uint8_t *data, size_t len)
{
    if (!s_connected || data == NULL || len == 0) {
        return -1;
    }
    if (len > SPP_MAX_DATA) {
        len = SPP_MAX_DATA;
    }
    return esp_spp_write(s_spp_handle, (int)len, (uint8_t *)data) == ESP_OK ? (int)len : -1;
}

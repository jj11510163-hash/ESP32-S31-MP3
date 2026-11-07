/*
 * bt_audio.h —— 经典蓝牙 A2DP 发射端 + AVRCP 绝对音量
 *
 * 依赖 IDF 的 Bluedroid 经典蓝牙栈；ESP32-S31 支持 BR/EDR
 * （IDF 内 components/bt/controller/lib_esp32s31/esp32s31-bt-lib/libbredr_app.a）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_ST_OFF = 0,
    BT_ST_INITING,
    BT_ST_READY,
    BT_ST_DISCOVERING,
    BT_ST_CONNECTING,
    BT_ST_CONNECTED,
    BT_ST_STREAMING,
    BT_ST_ERROR,
} bt_state_t;

#define BT_PEER_MAX 12

esp_err_t bt_audio_init(void);
bt_state_t bt_audio_state(void);
const char *bt_audio_state_name(bt_state_t st);

/* 搜索耳机（秒；0 使用一次约 13 秒的默认扫描） */
esp_err_t bt_audio_start_discovery(uint32_t seconds);
esp_err_t bt_audio_stop_discovery(void);
bool bt_audio_discovering(void);

int bt_audio_peer_count(void);
const char *bt_audio_peer_name(int idx);
const char *bt_audio_peer_addr(int idx);
const char *bt_audio_peer_name_by_addr(const char *addr);

esp_err_t bt_audio_connect(int idx);
esp_err_t bt_audio_connect_addr(const char *addr);
esp_err_t bt_audio_cancel_connect(void);
esp_err_t bt_audio_disconnect(void);
const char *bt_audio_current_addr(void);
bool bt_audio_addr_known(const char *addr);

typedef enum {
    BT_FORGET_IDLE = 0,
    BT_FORGET_PENDING,
    BT_FORGET_DONE,
    BT_FORGET_FAILED,
} bt_forget_status_t;

esp_err_t bt_audio_forget_addr(const char *addr);
bt_forget_status_t bt_audio_forget_status(void);

/* 绝对音量（AVRCP CT 的 SET_ABSOLUTE_VOLUME，0..127） */
esp_err_t bt_audio_set_absolute_volume(uint8_t vol);
uint8_t bt_audio_sink_volume(void);
bool bt_audio_sink_supports_absolute_volume(void);

const char *bt_audio_connected_name(void);
const char *bt_audio_local_name(void);
bool bt_audio_streaming(void);

/* ---------------- 预留：手机 APP 通道（SPP） ---------------- */
esp_err_t bt_app_link_init(void);
bool bt_app_link_connected(void);
int bt_app_link_send(const uint8_t *data, size_t len);
typedef void (*bt_app_link_rx_cb_t)(const uint8_t *data, size_t len);
void bt_app_link_set_rx_cb(bt_app_link_rx_cb_t cb);

/* ---------------- 编解码器偏好（自动/手动） ---------------- */
typedef enum {
    BT_CODEC_AUTO = 0, /* 对端支持 AAC 就用 AAC，否则 SBC */
    BT_CODEC_SBC = 1,  /* 强制 SBC */
    BT_CODEC_AAC = 2,  /* 强制 AAC（对端不支持则回退 SBC） */
} bt_codec_pref_t;

esp_err_t bt_audio_set_codec_pref(uint8_t pref);
uint8_t bt_audio_get_codec_pref(void);
/* 当前实际使用的编码：返回 "AAC" / "SBC" / "-" */
const char *bt_audio_active_codec_name(void);

#ifdef __cplusplus
}
#endif

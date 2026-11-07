/*
 * app_core.h —— 全局契约层
 *
 * 提供三样东西，所有模块只依赖这里，互相之间不直接耦合：
 *   1) 事件总线（跨任务/中断安全）
 *   2) 持久化设置（NVS）
 *   3) 输入抽象（按键/旋钮/触摸/手机 APP 都可以往这里投事件）
 *
 * 扩展新功能时，只需新增 app_evt_id_t 枚举值与载荷字段，不动既有代码。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * 1. 事件总线
 * ===================================================================== */

typedef enum {
    APP_EVT_NONE = 0,

    APP_EVT_SD_STATE,        /* .sd.mounted            SD 卡挂载状态     */
    APP_EVT_LIB_READY,       /* .lib.count             曲库扫描完成     */
    APP_EVT_LIB_PROGRESS,    /* .lib.count             扫描进度(已发现数) */

    APP_EVT_PLAYER_STATE,    /* .player.state          播放器状态       */
    APP_EVT_TRACK_CHANGED,   /* .track.index/.skip     切歌             */
    APP_EVT_TRACK_POS,       /* .pos.ms/.pos.dur_ms    播放进度(1Hz)    */
    APP_EVT_AUDIO_LEVEL,     /* .level.l/.level.r      音量柱(0..100)   */

    APP_EVT_BT_STATE,        /* .bt.state              蓝牙状态         */
    APP_EVT_BT_PEER,         /* .peer.name             对端设备名       */
    APP_EVT_BT_VOLUME,       /* .vol.value             绝对音量 0..127  */
    APP_EVT_BT_DISCOVERY,    /* .disc.active           正在搜索设备     */

    APP_EVT_INPUT,           /* .input.key/.input.value 输入事件        */
    APP_EVT_SETTINGS,        /* 设置已变更                              */

    APP_EVT_NET_STATE,       /* .net.ap_running/.net.clients  AP 状态   */
    APP_EVT_UPLOAD_PROGRESS, /* .upload.percent        上传进度         */
} app_evt_id_t;

typedef struct {
    app_evt_id_t id;
    union {
        struct {
            bool mounted;
        } sd;
        struct {
            int count;
        } lib;
        struct {
            int state; /* player_state_t */
        } player;
        struct {
            int index;
            int total;
            bool auto_advance;
        } track;
        struct {
            uint32_t ms;
            uint32_t dur_ms;
        } pos;
        struct {
            uint8_t l;
            uint8_t r;
        } level;
        struct {
            int state; /* bt_state_t */
        } bt;
        struct {
            char name[32];
        } peer;
        struct {
            uint8_t value;
        } vol;
        struct {
            bool active;
        } disc;
        struct {
            uint8_t key;
            int32_t value;
        } input;
        struct {
            bool ap_running;
            uint8_t clients;
        } net;
        struct {
            uint8_t percent;
        } upload;
    };
} app_evt_t;

esp_err_t app_events_init(void);

/* 任意任务上下文投递事件；wait_ms = 0 表示不等待 */
esp_err_t app_evt_post(const app_evt_t *evt, uint32_t wait_ms);

/* 便捷投递（只带 id，无载荷） */
esp_err_t app_evt_post_id(app_evt_id_t id);

/* 中断上下文中投递 */
esp_err_t app_evt_post_isr(const app_evt_t *evt, int32_t *hp_woken);

/* 阻塞等待事件；返回 false 表示超时 */
bool app_evt_wait(app_evt_t *out, uint32_t wait_ms);

/* 查询队列水位（调试用） */
uint32_t app_evt_pending(void);

/* ========================================================================
 * 2. 持久化设置
 * ===================================================================== */

typedef struct {
    uint8_t  volume;            /* 0..127 绝对音量（与 AVRCP 同一量纲）   */
    uint8_t  brightness;        /* 0..4   LCD 背光亮度档位                */
    uint8_t  eq;                /* 0..3   音效预设（预留：软件 EQ）       */
    uint8_t  auto_reconnect;    /* 0/1    开机自动回连上次耳机            */
    uint8_t  screen_timeout_s;  /* 0=常亮，其余为息屏秒数                */
    uint8_t  theme;             /* 0=线框 1=实心（两种 UI 风格）          */
    uint8_t  last_index;        /* 上次播放的曲目序号                    */
    uint8_t  shuffle;           /* 1=随机播放，0=按 repeat 模式播放        */
    uint8_t  repeat;            /* 1=全部循环，2=单曲循环                  */
    uint8_t  bt_codec;          /* 0=自动 1=SBC 2=AAC */
    uint8_t  reserved[6];
    char     last_peer_addr[18];/* "aa:bb:cc:dd:ee:ff"                    */
    char     last_peer_name[32];
} app_settings_t;

esp_err_t app_settings_init(void);
const app_settings_t *app_settings_get(void);

/* 直接改 + 落盘 + 广播 APP_EVT_SETTINGS */
esp_err_t app_settings_update(const app_settings_t *new_cfg);
esp_err_t app_settings_save(void);

/* 常用快捷操作（内部广播相应事件） */
esp_err_t app_settings_set_volume(uint8_t vol);     /* 0..127 */
esp_err_t app_settings_volume_step(int8_t delta, uint8_t step);
esp_err_t app_settings_set_brightness(uint8_t level);

/* 是否有未落盘的改动（关机/休眠前调用 app_settings_save） */
bool app_settings_dirty(void);

/* ========================================================================
 * 3. 输入抽象
 * ===================================================================== */

typedef enum {
    APP_KEY_PREV = 0,
    APP_KEY_NEXT,
    APP_KEY_PLAY,     /* 播放/暂停   */
    APP_KEY_ENTER,    /* 确认        */
    APP_KEY_BACK,     /* 返回        */
    APP_KEY_MENU,     /* 进菜单      */
    APP_KEY_ROT_CW,   /* 旋钮右转    */
    APP_KEY_ROT_CCW,  /* 旋钮左转    */
    APP_KEY_UP,
    APP_KEY_DOWN,
    APP_KEY_KEY0_SINGLE,
    APP_KEY_KEY0_DOUBLE,
    APP_KEY_KEY0_TRIPLE,
    APP_KEY_KEY0_LONG,
    APP_KEY_PAGE_PREV,
    APP_KEY_PAGE_NEXT,
    APP_KEY_KEY0_QUAD,
    APP_KEY_MAX
} app_key_t;

/* 预留：后续编码器 / 电容触摸 / 手机 APP 都可以调用它注输入 */
void app_input_post(app_key_t key, int32_t value);

/* 预留：外部后端注册（例如触摸驱动），返回 true 表示已消费 */
typedef bool (*app_input_handler_t)(app_key_t key, int32_t value);
esp_err_t app_input_register_handler(app_input_handler_t handler);

/* 板载 KEY0/BOOT 键（IO61）：单击、双击、三击、四击和长按 */
esp_err_t app_input_init(void);

/* 串口遥控（WASDRF），在 idf_monitor 里直接敲键 */
esp_err_t app_serial_input_start(void);

const char *app_key_name(app_key_t key);

#ifdef __cplusplus
}
#endif

/*
 * bt_audio.c —�?经典蓝牙：控制器 + Bluedroid + GAP 发现 + A2DP Source + AVRCP CT
 *
 * 绝对音量：AVRCP CT 侧用 esp_avrc_ct_send_set_absolute_volume_cmd() 直接改耳机音量�? * 并通过 ESP_AVRC_RN_VOLUME_CHANGE 通知同步耳机按键改音量的结果�? */
#include "bt_audio.h"
#include "bt_stream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_core.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "music.h"

static const char *TAG = "bt";
static const char *LOCAL_NAME = "S31-Player";

#define BT_TL_CAPS 0
#define BT_TL_VOLUME_NOTIFY 1

/* ------------------------------------------------------------------ */
/* 状�?                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[32];
    char addr[18];
} peer_t;

static peer_t s_peers[BT_PEER_MAX];
static int s_peer_count;

static volatile bt_state_t s_state = BT_ST_OFF;
static esp_bd_addr_t s_peer_bda;
static char s_peer_name[32];
static char s_current_addr[18];

static volatile bool s_disc_active;
static volatile bool s_connect_pending;
static volatile bool s_connect_started;
static volatile bool s_cancel_requested;
static volatile uint32_t s_connect_deadline_ms;
static volatile uint32_t s_cancel_deadline_ms;
static esp_bd_addr_t s_reject_late_bda;
static volatile bool s_reject_late;
static volatile uint32_t s_reject_late_deadline_ms;
static volatile int s_audio_codec = -1;
static esp_a2d_mcc_t s_stream_mcc;
static volatile uint8_t s_codec_pref = BT_CODEC_AUTO;
static volatile uint8_t s_sep_ready_mask;
static volatile uint8_t s_sink_aac_obj_types;
static volatile bool s_pref_pending;
static volatile uint32_t s_pref_deadline_ms;
static esp_bd_addr_t s_forget_bda;
static volatile bt_forget_status_t s_forget_status = BT_FORGET_IDLE;
static volatile bool s_forget_wait_disconnect;
static volatile bool s_forget_bond_requested;
static volatile int s_forget_remove_result = -1;
static volatile uint32_t s_forget_deadline_ms;
static esp_bd_addr_t s_forgotten[BT_PEER_MAX];
static uint8_t s_forgotten_count;
static portMUX_TYPE s_forgotten_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_sink_vol = 127;
static volatile bool s_rn_supported;
static volatile bool s_avrc_ct_connected;
static volatile bool s_abs_supported;
static uint32_t s_volume_tl_counter;
static volatile bool s_media_started;
static uint32_t s_stream_generation;
static bool s_fallback_sbc;
static bool s_fallback_apply_queued;
static bool s_fallback_wait_codec;
static uint32_t s_fallback_deadline_ms;
static esp_avrc_rn_evt_cap_mask_t s_rn_caps;

static QueueHandle_t s_work_q;
static esp_a2d_conn_hdl_t s_conn_hdl;
static uint16_t s_audio_mtu;
static TaskHandle_t s_work_task;
static volatile uint32_t s_disc_deadline_ms;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */
static void bda2str(const esp_bd_addr_t bda, char *out, size_t n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool str2bda(const char *s, esp_bd_addr_t out)
{
    unsigned v[6];
    int end = 0;
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) != 6 ||
        s[end] != '\0') {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static bool bda_equal(const esp_bd_addr_t a, const esp_bd_addr_t b)
{
    return memcmp(a, b, ESP_BD_ADDR_LEN) == 0;
}

static bool usable_peer_name(const char *name, const esp_bd_addr_t bda)
{
    esp_bd_addr_t parsed;
    return name && name[0] && !(str2bda(name, parsed) && bda_equal(parsed, bda));
}

static bool forgotten_contains(const esp_bd_addr_t bda)
{
    bool found = false;
    portENTER_CRITICAL(&s_forgotten_lock);
    for (uint8_t i = 0; i < s_forgotten_count; i++) {
        if (bda_equal(s_forgotten[i], bda)) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_forgotten_lock);
    return found;
}

static void forgotten_add(const esp_bd_addr_t bda)
{
    portENTER_CRITICAL(&s_forgotten_lock);
    for (uint8_t i = 0; i < s_forgotten_count; i++) {
        if (bda_equal(s_forgotten[i], bda)) {
            portEXIT_CRITICAL(&s_forgotten_lock);
            return;
        }
    }
    if (s_forgotten_count == BT_PEER_MAX) {
        memmove(s_forgotten, s_forgotten + 1, (BT_PEER_MAX - 1) * sizeof(s_forgotten[0]));
        s_forgotten_count--;
    }
    memcpy(s_forgotten[s_forgotten_count++], bda, ESP_BD_ADDR_LEN);
    portEXIT_CRITICAL(&s_forgotten_lock);
}

static void forgotten_remove(const esp_bd_addr_t bda)
{
    portENTER_CRITICAL(&s_forgotten_lock);
    for (uint8_t i = 0; i < s_forgotten_count; i++) {
        if (bda_equal(s_forgotten[i], bda)) {
            s_forgotten_count--;
            memmove(s_forgotten + i, s_forgotten + i + 1,
                    (s_forgotten_count - i) * sizeof(s_forgotten[0]));
            break;
        }
    }
    portEXIT_CRITICAL(&s_forgotten_lock);
}

static void notify_bt_state(void)
{
    app_evt_t evt = {.id = APP_EVT_BT_STATE};
    evt.bt.state = (int)s_state;
    app_evt_post(&evt, 0);
}

static esp_err_t is_bonded(const esp_bd_addr_t bda, bool *bonded)
{
    *bonded = false;
    int count = esp_bt_gap_get_bond_device_num();
    if (count < 0) {
        return ESP_FAIL;
    }
    if (count == 0) {
        return ESP_OK;
    }
    esp_bd_addr_t *devices = malloc((size_t)count * sizeof(*devices));
    if (!devices) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_bt_gap_get_bond_device_list(&count, devices);
    if (err == ESP_OK) {
        for (int i = 0; i < count; i++) {
            if (bda_equal(devices[i], bda)) {
                *bonded = true;
                break;
            }
        }
    }
    free(devices);
    return err;
}

static void set_state(bt_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    app_evt_t evt = {.id = APP_EVT_BT_STATE};
    evt.bt.state = (int)st;
    app_evt_post(&evt, 0);
}

const char *bt_audio_state_name(bt_state_t st)
{
    switch (st) {
    case BT_ST_OFF:
        return "BT OFF";
    case BT_ST_INITING:
        return "INIT...";
    case BT_ST_READY:
        return "SEARCHING";
    case BT_ST_DISCOVERING:
        return "SCANNING";
    case BT_ST_CONNECTING:
        return "LINKING";
    case BT_ST_CONNECTED:
        return "CONNECTED";
    case BT_ST_STREAMING:
        return "PLAYING";
    default:
        return "BT ERROR";
    }
}

bt_state_t bt_audio_state(void)
{
    return s_state;
}

bool bt_audio_discovering(void)
{
    return s_disc_active;
}

const char *bt_audio_local_name(void)
{
    return LOCAL_NAME;
}

const char *bt_audio_connected_name(void)
{
    return usable_peer_name(s_peer_name, s_peer_bda) ? s_peer_name : "蓝牙耳机";
}

const char *bt_audio_current_addr(void)
{
    return (s_state == BT_ST_CONNECTING || s_state == BT_ST_CONNECTED ||
            s_state == BT_ST_STREAMING) ? s_current_addr : "";
}

bool bt_audio_addr_known(const char *addr)
{
    esp_bd_addr_t bda, saved;
    if (!str2bda(addr, bda)) {
        return false;
    }
    const char *last = app_settings_get()->last_peer_addr;
    if (str2bda(last, saved) && bda_equal(bda, saved)) {
        return true;
    }
    bool bonded = false;
    return is_bonded(bda, &bonded) == ESP_OK && bonded;
}

bt_forget_status_t bt_audio_forget_status(void)
{
    return s_forget_status;
}

bool bt_audio_streaming(void)
{
    return s_media_started;
}

int bt_audio_peer_count(void)
{
    return s_peer_count;
}

const char *bt_audio_peer_name(int idx)
{
    if (idx < 0 || idx >= s_peer_count) {
        return "";
    }
    esp_bd_addr_t bda;
    return str2bda(s_peers[idx].addr, bda) &&
           usable_peer_name(s_peers[idx].name, bda) ? s_peers[idx].name : "未知设备";
}

const char *bt_audio_peer_addr(int idx)
{
    if (idx < 0 || idx >= s_peer_count) {
        return "";
    }
    return s_peers[idx].addr;
}

const char *bt_audio_peer_name_by_addr(const char *addr)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].addr, addr) == 0) {
            return s_peers[i].name;
        }
    }
    return "";
}

static void add_peer(const esp_bd_addr_t bda, const char *name)
{
    char addr[18];
    bda2str(bda, addr, sizeof(addr));
    if (!usable_peer_name(name, bda)) {
        const app_settings_t *cfg = app_settings_get();
        name = strcmp(cfg->last_peer_addr, addr) == 0 &&
               usable_peer_name(cfg->last_peer_name, bda) ? cfg->last_peer_name : NULL;
    }
    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].addr, addr) == 0) {
            if (name && strcmp(s_peers[i].name, name) != 0) {
                snprintf(s_peers[i].name, sizeof(s_peers[i].name), "%.*s",
                         (int)sizeof(s_peers[i].name) - 1, name);
                app_evt_post_id(APP_EVT_BT_PEER);
            }
            return;
        }
    }
    if (s_peer_count >= BT_PEER_MAX) {
        return;
    }
    snprintf(s_peers[s_peer_count].addr, sizeof(s_peers[s_peer_count].addr), "%s", addr);
    snprintf(s_peers[s_peer_count].name, sizeof(s_peers[s_peer_count].name), "%s",
             (name && name[0]) ? name : addr);
    s_peer_count++;
    ESP_LOGI(TAG, "peer + %s [%s] (total %d)", s_peers[s_peer_count - 1].name, addr, s_peer_count);
    app_evt_t evt = {.id = APP_EVT_BT_PEER};
    snprintf(evt.peer.name, sizeof(evt.peer.name), "%s", s_peers[s_peer_count - 1].name);
    app_evt_post(&evt, 0);
}

/* ------------------------------------------------------------------ */
/* A2DP Source                                                         */
/* ------------------------------------------------------------------ */
static void build_aac_mcc(esp_a2d_mcc_t *mcc, uint8_t object_type)
{
    memset(mcc, 0, sizeof(*mcc));
    mcc->type = ESP_A2D_MCT_M24;
    mcc->cie.m24_info.obj_type = object_type;
    mcc->cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
    mcc->cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
    mcc->cie.m24_info.br1 = 0x02;
    mcc->cie.m24_info.br2 = 0x71;
}

static void build_sbc_mcc(esp_a2d_mcc_t *mcc, bool capability)
{
    memset(mcc, 0, sizeof(*mcc));
    mcc->type = ESP_A2D_MCT_SBC;
    mcc->cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    mcc->cie.sbc_info.ch_mode = capability ?
        (ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO | ESP_A2D_SBC_CIE_CH_MODE_STEREO |
         ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL) :
        ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc->cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc->cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc->cie.sbc_info.alloc_mthd = capability ?
        (ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS | ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR) :
        ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc->cie.sbc_info.min_bitpool = 2;
    mcc->cie.sbc_info.max_bitpool = 53;
}

enum {
    WORK_A2D_EVT = 1,
    WORK_REGISTER_CODECS,
    WORK_BT_READY,
    WORK_START_MEDIA,
    WORK_CHECK_READY,
    WORK_APPLY_CODEC,
    WORK_STREAM_START,
    WORK_SAVE_PEER,
    WORK_AVRC_PLAY,
    WORK_AVRC_PAUSE,
    WORK_AVRC_NEXT,
    WORK_AVRC_PREV,
    WORK_AVRC_VOL_UP,
    WORK_AVRC_VOL_DOWN,
};

static void a2d_post(uint16_t evt)
{
    if (s_work_q && xQueueSend(s_work_q, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BT work queue full, dropped event %u", evt);
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_PROF_STATE_EVT:
        if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
            a2d_post(WORK_REGISTER_CODECS);
        } else {
            ESP_LOGE(TAG, "A2DP profile initialization failed");
            set_state(BT_ST_ERROR);
        }
        break;
    case ESP_A2D_SEP_REG_STATE_EVT:
        ESP_LOGI(TAG, "A2DP endpoint %u registration=%d",
                 param->a2d_sep_reg_stat.seid, param->a2d_sep_reg_stat.reg_state);
        if (s_state != BT_ST_INITING) break;
        if (param->a2d_sep_reg_stat.seid >= 2 ||
            param->a2d_sep_reg_stat.reg_state != ESP_A2D_SEP_REG_SUCCESS) {
            set_state(BT_ST_ERROR);
        } else {
            s_sep_ready_mask |= (uint8_t)(1u << param->a2d_sep_reg_stat.seid);
            if (s_sep_ready_mask == 3) a2d_post(WORK_BT_READY);
        }
        break;
    case ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT: {
        const esp_a2d_sep_mcc_t *seps = param->a2d_report_snk_all_codec_caps_stat.sep_mcc;
        uint8_t count = param->a2d_report_snk_all_codec_caps_stat.sep_num;
        uint8_t objects = 0;
        for (uint8_t i = 0; seps && i < count; i++) {
            if (seps[i].mcc.type == ESP_A2D_MCT_M24) {
                uint8_t type = seps[i].mcc.cie.m24_info.obj_type;
                objects |= type;
                ESP_LOGI(TAG, "sink AAC endpoint=%u objects=0x%02x freq1=0x%02x ch=0x%02x",
                         seps[i].seid, type, seps[i].mcc.cie.m24_info.samp_freq1,
                         seps[i].mcc.cie.m24_info.ch);
            }
        }
        s_sink_aac_obj_types = objects;
        ESP_LOGI(TAG, "sink endpoints=%u AAC objects=0x%02x", count, objects);
        break;
    }
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "A2DP state=%d reason=%d peer=%02x:%02x:%02x:%02x:%02x:%02x",
                 param->conn_stat.state, param->conn_stat.disc_rsn,
                 param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                 param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                 param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING &&
            (s_state == BT_ST_READY || s_state == BT_ST_DISCOVERING)) {
            /* An earbud may initiate the link before our reconnect request. Its
             * codec config can arrive before the CONNECTED event. */
            memcpy(s_peer_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            bda2str(s_peer_bda, s_current_addr, sizeof(s_current_addr));
            s_audio_codec = -1;
            s_sink_aac_obj_types = 0;
            set_state(BT_ST_CONNECTING);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            if (s_cancel_requested ||
                (s_reject_late && bda_equal(param->conn_stat.remote_bda, s_reject_late_bda)) ||
                forgotten_contains(param->conn_stat.remote_bda)) {
                ESP_LOGW(TAG, "unwanted A2DP connection; disconnecting");
                esp_a2d_source_disconnect(param->conn_stat.remote_bda);
                break;
            }
            if (s_forget_status == BT_FORGET_PENDING &&
                bda_equal(param->conn_stat.remote_bda, s_forget_bda)) {
                s_forget_wait_disconnect = true;
                esp_a2d_source_disconnect(param->conn_stat.remote_bda);
                break;
            }
            memcpy(s_peer_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            bda2str(s_peer_bda, s_current_addr, sizeof(s_current_addr));
            s_connect_pending = false;
            s_connect_started = false;
            s_connect_deadline_ms = 0;
            const app_settings_t *cfg = app_settings_get();
            if (!usable_peer_name(s_peer_name, s_peer_bda) &&
                strcmp(cfg->last_peer_addr, s_current_addr) == 0 &&
                usable_peer_name(cfg->last_peer_name, s_peer_bda)) {
                snprintf(s_peer_name, sizeof(s_peer_name), "%s", cfg->last_peer_name);
            }
            if (!usable_peer_name(s_peer_name, s_peer_bda)) {
                bda2str(s_peer_bda, s_peer_name, sizeof(s_peer_name));
                esp_err_t err = esp_bt_gap_read_remote_name(s_peer_bda);
                if (err != ESP_OK)
                    ESP_LOGW(TAG, "read remote name failed to start: %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "A2DP connected");
            set_state(BT_ST_CONNECTED);
            a2d_post(WORK_SAVE_PEER);
            s_conn_hdl = param->conn_stat.conn_hdl;
            s_audio_mtu = param->conn_stat.audio_mtu;
            s_stream_generation++;
            s_fallback_sbc = false;
            s_fallback_apply_queued = false;
            s_fallback_wait_codec = false;
            s_fallback_deadline_ms = 0;
            ESP_LOGI(TAG, "link up: hdl=%d mtu=%u", (int)s_conn_hdl, s_audio_mtu);
            a2d_post(WORK_APPLY_CODEC);
            if (s_avrc_ct_connected) {
                bt_audio_set_absolute_volume(app_settings_get()->volume);
            }
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            if (!bda_equal(param->conn_stat.remote_bda, s_peer_bda) ||
                (s_state != BT_ST_CONNECTING && s_state != BT_ST_CONNECTED &&
                 s_state != BT_ST_STREAMING)) {
                break;
            }
            s_connect_pending = false;
            s_connect_started = false;
            s_cancel_requested = false;
            s_cancel_deadline_ms = 0;
            s_connect_deadline_ms = 0;
            s_media_started = false;
            s_pref_pending = false;
            bt_stream_stop();
            s_stream_generation++;
            s_fallback_sbc = false;
            s_fallback_apply_queued = false;
            s_fallback_wait_codec = false;
            s_fallback_deadline_ms = 0;
            s_conn_hdl = 0;
            s_audio_mtu = 0;
            s_audio_codec = -1;
            s_sink_aac_obj_types = 0;
            s_current_addr[0] = '\0';
            s_peer_name[0] = '\0';
            if (s_forget_status == BT_FORGET_PENDING &&
                bda_equal(param->conn_stat.remote_bda, s_forget_bda)) {
                s_forget_wait_disconnect = false;
            }
            set_state(BT_ST_READY);
            notify_bt_state();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state=%d codec=%d", param->audio_stat.state,
                 s_audio_codec);
        if (s_cancel_requested ||
            (s_forget_status == BT_FORGET_PENDING && s_forget_wait_disconnect)) {
            break;
        }
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_media_started = true;
            set_state(BT_ST_STREAMING);
            a2d_post(WORK_STREAM_START);
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            s_media_started = false;
            bt_stream_stop();
            set_state(BT_ST_CONNECTED);
            if (s_fallback_sbc && !s_fallback_apply_queued) {
                s_fallback_apply_queued = true;
                a2d_post(WORK_APPLY_CODEC);
            }
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        ESP_LOGI(TAG, "A2DP media cmd=%d ack=%d", param->media_ctrl_stat.cmd,
                 param->media_ctrl_stat.status);
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            a2d_post(WORK_START_MEDIA);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                   s_fallback_sbc) {
            if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGE(TAG, "cannot suspend AAC for SBC fallback; disconnecting");
                esp_a2d_source_disconnect(s_peer_bda);
            } else if (!s_fallback_apply_queued) {
                s_fallback_apply_queued = true;
                a2d_post(WORK_APPLY_CODEC);
            }
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "audio cfg codec=%d", param->audio_cfg.mcc.type);
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_M24)
            ESP_LOGI(TAG, "AAC config object=0x%02x freq1=0x%02x ch=0x%02x",
                     param->audio_cfg.mcc.cie.m24_info.obj_type,
                     param->audio_cfg.mcc.cie.m24_info.samp_freq1,
                     param->audio_cfg.mcc.cie.m24_info.ch);
        if (!s_cancel_requested && s_state != BT_ST_READY) {
            s_audio_codec = param->audio_cfg.mcc.type;
            s_stream_mcc = param->audio_cfg.mcc;
            notify_bt_state();
            if (s_media_started) a2d_post(WORK_STREAM_START);
            if (s_fallback_wait_codec && s_audio_codec == ESP_A2D_MCT_SBC) {
                s_fallback_wait_codec = false;
                a2d_post(WORK_CHECK_READY);
            }
        }
        break;
    case ESP_A2D_SRC_SET_PREF_MCC_EVT:
        if (s_pref_pending && param->a2d_set_pref_mcc_stat.conn_hdl == s_conn_hdl) {
            ESP_LOGI(TAG, "codec preference result=%d actual=%d",
                     param->a2d_set_pref_mcc_stat.set_status, s_audio_codec);
            s_pref_pending = false;
            if (s_fallback_sbc &&
                param->a2d_set_pref_mcc_stat.set_status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "SBC fallback reconfiguration failed; disconnecting");
                esp_a2d_source_disconnect(s_peer_bda);
            } else if (s_fallback_sbc && s_audio_codec != ESP_A2D_MCT_SBC) {
                s_fallback_wait_codec = true;
            } else {
                a2d_post(WORK_CHECK_READY);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* AVRCP CT（含绝对音量�?                                             */
/* ------------------------------------------------------------------ */
static void avrc_volume_changed_notify(void)
{
    if (s_rn_supported) {
        esp_err_t err = esp_avrc_ct_send_register_notification_cmd(
            BT_TL_VOLUME_NOTIFY, ESP_AVRC_RN_VOLUME_CHANGE, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register volume notification failed: %s", esp_err_to_name(err));
        }
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRCP connected");
            s_avrc_ct_connected = true;
            esp_err_t err = esp_avrc_ct_send_get_rn_capabilities_cmd(BT_TL_CAPS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "get AVRCP capabilities failed: %s", esp_err_to_name(err));
            }
            if (s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING) {
                bt_audio_set_absolute_volume(app_settings_get()->volume);
            }
        } else {
            ESP_LOGI(TAG, "AVRCP disconnected");
            s_avrc_ct_connected = false;
            s_rn_supported = false;
            s_abs_supported = false;
            s_rn_caps.bits = 0;
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_rn_caps.bits = param->get_rn_caps_rsp.evt_set.bits;
        s_rn_supported = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_rn_caps,
                                                            ESP_AVRC_RN_VOLUME_CHANGE);
        ESP_LOGI(TAG, "AVRCP RN caps=0x%x volume_notify=%d", s_rn_caps.bits, s_rn_supported);
        if (s_rn_supported) {
            avrc_volume_changed_notify();
        }
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            uint8_t v = param->change_ntf.event_parameter.volume;
            if (v > 127) {
                v = 127;
            }
            s_sink_vol = v;
            s_abs_supported = true;
            app_settings_set_volume(v);
            ESP_LOGI(TAG, "sink volume changed -> %d", v);
            avrc_volume_changed_notify();
        }
        break;
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        s_sink_vol = param->set_volume_rsp.volume;
        s_abs_supported = true;
        ESP_LOGI(TAG, "abs volume ack -> %d", s_sink_vol);
        break;
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGI(TAG, "AVRCP CT response key=0x%02x state=%u rsp=%d",
                 param->psth_rsp.key_code, param->psth_rsp.key_state, param->psth_rsp.rsp_code);
        break;
    default:
        break;
    }
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_PROF_STATE_EVT:
        if (param->avrc_tg_init_stat.state == ESP_AVRC_INIT_SUCCESS) {
            esp_avrc_psth_bit_mask_t commands = {0};
            const esp_avrc_pt_cmd_t supported[] = {
                ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE,
                ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD,
                ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_VOL_DOWN,
            };
            for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
                esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &commands, supported[i]);
            }
            esp_err_t err = esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD,
                                                             &commands);
            ESP_LOGI(TAG, "AVRCP TG command filter: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "AVRCP TG init state=%d", param->avrc_tg_init_stat.state);
        }
        break;
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP TG %s", param->conn_stat.connected ? "connected" : "disconnected");
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        ESP_LOGI(TAG, "headset key=0x%02x state=%u", param->psth_cmd.key_code,
                 param->psth_cmd.key_state);
        if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
            break;
        }
        switch (param->psth_cmd.key_code) {
        case ESP_AVRC_PT_CMD_PLAY:
            a2d_post(WORK_AVRC_PLAY);
            break;
        case ESP_AVRC_PT_CMD_PAUSE:
            a2d_post(WORK_AVRC_PAUSE);
            break;
        case ESP_AVRC_PT_CMD_FORWARD:
            a2d_post(WORK_AVRC_NEXT);
            break;
        case ESP_AVRC_PT_CMD_BACKWARD:
            a2d_post(WORK_AVRC_PREV);
            break;
        case ESP_AVRC_PT_CMD_VOL_UP:
            a2d_post(WORK_AVRC_VOL_UP);
            break;
        case ESP_AVRC_PT_CMD_VOL_DOWN:
            a2d_post(WORK_AVRC_VOL_DOWN);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

esp_err_t bt_audio_set_absolute_volume(uint8_t vol)
{
    if (vol > 127) {
        vol = 127;
    }
    if (!s_avrc_ct_connected || (s_state != BT_ST_CONNECTED && s_state != BT_ST_STREAMING)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tl = 2 + (__atomic_fetch_add(&s_volume_tl_counter, 1, __ATOMIC_RELAXED) % 14);
    esp_err_t err = esp_avrc_ct_send_set_absolute_volume_cmd(tl, vol);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set absolute volume %u failed: %s", vol, esp_err_to_name(err));
    }
    return err;
}

uint8_t bt_audio_sink_volume(void)
{
    return s_sink_vol;
}

bool bt_audio_sink_supports_absolute_volume(void)
{
    return s_abs_supported;
}

/* ------------------------------------------------------------------ */
/* GAP                                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t launch_pending_connect(void)
{
    if (!s_connect_pending || s_cancel_requested || music_update_mode()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_connect_pending = false;
    s_connect_started = true;
    ESP_LOGI(TAG, "A2DP connect -> %s", s_peer_name);
    esp_err_t err = esp_a2d_source_connect(s_peer_bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "A2DP connect start failed: %s", esp_err_to_name(err));
        s_connect_started = false;
        s_connect_deadline_ms = 0;
        s_current_addr[0] = '\0';
        set_state(BT_ST_READY);
    }
    return err;
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (!s_disc_active) {
            break;
        }
        char name[64] = {0};
        uint32_t cod = 0;
        bool has_audio = false;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int n = p->len < (int)sizeof(name) - 1 ? p->len : (int)sizeof(name) - 1;
                memcpy(name, p->val, (size_t)n);
                name[n] = '\0';
            } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
                uint8_t len = 0;
                uint8_t *n = esp_bt_gap_resolve_eir_data((uint8_t *)p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (!n || !len)
                    n = esp_bt_gap_resolve_eir_data((uint8_t *)p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                if (n && len && !name[0]) {
                    int c = len < (int)sizeof(name) - 1 ? len : (int)sizeof(name) - 1;
                    memcpy(name, n, (size_t)c);
                    name[c] = '\0';
                }
            } else if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
                cod = *(uint32_t *)p->val;
            }
        }
        /* 只保留可能是耳机/音箱的设备（Audio 大类�?*/
        if (esp_bt_gap_is_valid_cod(cod)) {
            uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
            if (major == ESP_BT_COD_MAJOR_DEV_AV || (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
                has_audio = true;
            }
        }
        if (has_audio || name[0]) {
            add_peer(param->disc_res.bda, name);
        }
        break;
    }
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
        const esp_bd_addr_t *bda = &param->read_rmt_name.bda;
        const char *name = (const char *)param->read_rmt_name.rmt_name;
        if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS ||
            !usable_peer_name(name, *bda)) {
            ESP_LOGW(TAG, "remote name unavailable (status=0x%x)", param->read_rmt_name.stat);
            break;
        }
        char addr[18];
        bda2str(*bda, addr, sizeof(addr));
        for (int i = 0; i < s_peer_count; i++) {
            if (strcmp(s_peers[i].addr, addr) == 0) {
                snprintf(s_peers[i].name, sizeof(s_peers[i].name), "%.*s",
                         (int)sizeof(s_peers[i].name) - 1, name);
                app_evt_post_id(APP_EVT_BT_PEER);
                break;
            }
        }
        if (s_current_addr[0] && bda_equal(*bda, s_peer_bda) &&
            (s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING)) {
            snprintf(s_peer_name, sizeof(s_peer_name), "%.*s",
                     (int)sizeof(s_peer_name) - 1, name);
            ESP_LOGI(TAG, "remote name: %s [%s]", s_peer_name, addr);
            notify_bt_state();
            a2d_post(WORK_SAVE_PEER);
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_disc_active = false;
            s_disc_deadline_ms = 0;
            if (s_connect_pending) {
                launch_pending_connect();
            } else if (s_state == BT_ST_DISCOVERING) {
                set_state(BT_ST_READY);
            }
            ESP_LOGI(TAG, "discovery stopped (%d peers)", s_peer_count);
        } else {
            ESP_LOGI(TAG, "discovery started");
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "auth %s status=0x%x: %s",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "fail",
                 param->auth_cmpl.stat, param->auth_cmpl.device_name);
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS &&
            forgotten_contains(param->auth_cmpl.bda) &&
            !(s_forget_status == BT_FORGET_PENDING &&
              bda_equal(param->auth_cmpl.bda, s_forget_bda))) {
            esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
        }
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL connect status=0x%x handle=%u", param->acl_conn_cmpl_stat.stat,
                 param->acl_conn_cmpl_stat.handle);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL disconnect reason=0x%x handle=%u", param->acl_disconn_cmpl_stat.reason,
                 param->acl_disconn_cmpl_stat.handle);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        bool accept = !forgotten_contains(param->pin_req.bda);
        esp_bt_gap_pin_reply(param->pin_req.bda, accept, accept ? 4 : 0, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda,
                                     !forgotten_contains(param->cfm_req.bda));
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "pairing passkey %06" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        if (s_forget_status == BT_FORGET_PENDING && s_forget_bond_requested &&
            bda_equal(param->remove_bond_dev_cmpl.bda, s_forget_bda)) {
            s_forget_remove_result = param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS ? 0 : 1;
        }
        break;
    default:
        break;
    }
}

esp_err_t bt_audio_start_discovery(uint32_t seconds)
{
    if (music_update_mode()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == BT_ST_DISCOVERING) {
        return ESP_OK;
    }
    if (s_state != BT_ST_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    s_peer_count = 0;
    uint32_t inquiry_len = seconds >= 62 ? 48 :
                           seconds ? (seconds * 100 + 127) / 128 : 10;
    if (inquiry_len < 1) {
        inquiry_len = 1;
    }
    if (inquiry_len > 48) {
        inquiry_len = 48;
    }
    s_disc_active = true;
    s_disc_deadline_ms = seconds ?
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + seconds * 1000 : 0;
    set_state(BT_ST_DISCOVERING);
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (uint8_t)inquiry_len, 0);
    if (err != ESP_OK) {
        s_disc_active = false;
        s_disc_deadline_ms = 0;
        set_state(BT_ST_READY);
    }
    return err;
}

esp_err_t bt_audio_stop_discovery(void)
{
    if (s_disc_active) {
        s_disc_deadline_ms = 0;
        esp_bt_gap_cancel_discovery();
    }
    return ESP_OK;
}

static esp_err_t request_connect(const esp_bd_addr_t bda, const char *name)
{
    if (music_update_mode() ||
        (s_state != BT_ST_READY && s_state != BT_ST_DISCOVERING) ||
        s_forget_status == BT_FORGET_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_peer_bda, bda, ESP_BD_ADDR_LEN);
    bda2str(bda, s_current_addr, sizeof(s_current_addr));
    forgotten_remove(bda);
    if (s_reject_late && bda_equal(bda, s_reject_late_bda)) {
        s_reject_late = false;
    }
    const app_settings_t *cfg = app_settings_get();
    if (!usable_peer_name(name, bda) && strcmp(cfg->last_peer_addr, s_current_addr) == 0 &&
        usable_peer_name(cfg->last_peer_name, bda))
        name = cfg->last_peer_name;
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", usable_peer_name(name, bda) ? name : s_current_addr);
    ESP_LOGI(TAG, "selected peer %s [%02x:%02x:%02x:%02x:%02x:%02x]",
             s_peer_name, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    s_connect_pending = true;
    s_connect_started = false;
    s_cancel_requested = false;
    s_audio_codec = -1;
    s_sink_aac_obj_types = 0;
    s_connect_deadline_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 20000;
    set_state(BT_ST_CONNECTING);
    if (s_disc_active) {
        s_disc_deadline_ms = 0;
        esp_err_t err = esp_bt_gap_cancel_discovery();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cancel discovery before connect: %s", esp_err_to_name(err));
            if (!s_disc_active && s_connect_pending) {
                return launch_pending_connect();
            }
        }
        return ESP_OK;
    }
    return launch_pending_connect();
}

esp_err_t bt_audio_connect(int idx)
{
    esp_bd_addr_t bda;
    if (idx < 0 || idx >= s_peer_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!str2bda(s_peers[idx].addr, bda)) {
        return ESP_ERR_INVALID_ARG;
    }
    return request_connect(bda, s_peers[idx].name);
}

esp_err_t bt_audio_connect_addr(const char *addr)
{
    esp_bd_addr_t bda;
    if (!str2bda(addr, bda)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *name = bt_audio_peer_name_by_addr(addr);
    if (!usable_peer_name(name, bda)) {
        const app_settings_t *cfg = app_settings_get();
        name = strcmp(addr, cfg->last_peer_addr) == 0 &&
               usable_peer_name(cfg->last_peer_name, bda) ?
               cfg->last_peer_name : addr;
    }
    return request_connect(bda, name);
}

esp_err_t bt_audio_disconnect(void)
{
    if (s_state != BT_ST_CONNECTED && s_state != BT_ST_STREAMING) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_a2d_source_disconnect(s_peer_bda);
}

esp_err_t bt_audio_cancel_connect(void)
{
    if (s_state != BT_ST_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cancel_requested) {
        return ESP_OK;
    }
    s_connect_deadline_ms = 0;
    if (s_connect_pending && !s_connect_started) {
        s_connect_pending = false;
        if (s_disc_active) {
            esp_bt_gap_cancel_discovery();
        }
        s_current_addr[0] = '\0';
        s_peer_name[0] = '\0';
        set_state(BT_ST_READY);
        return ESP_OK;
    }
    s_cancel_requested = true;
    memcpy(s_reject_late_bda, s_peer_bda, ESP_BD_ADDR_LEN);
    s_reject_late = true;
    s_reject_late_deadline_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 60000;
    s_cancel_deadline_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 20000;
    esp_err_t err = esp_a2d_source_disconnect(s_peer_bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cancel pending A2DP connection: %s; waiting for callback", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t bt_audio_forget_addr(const char *addr)
{
    esp_bd_addr_t bda;
    if (!str2bda(addr, bda)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_forget_status == BT_FORGET_PENDING || s_state == BT_ST_OFF ||
        s_state == BT_ST_INITING || s_state == BT_ST_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_forget_bda, bda, ESP_BD_ADDR_LEN);
    forgotten_add(bda);
    s_forget_wait_disconnect = false;
    s_forget_bond_requested = false;
    s_forget_remove_result = -1;
    s_forget_deadline_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 25000;
    s_forget_status = BT_FORGET_PENDING;
    notify_bt_state();

    if (s_state == BT_ST_DISCOVERING) {
        bt_audio_stop_discovery();
    }

    if (bda_equal(bda, s_peer_bda) && s_state == BT_ST_CONNECTING) {
        s_forget_wait_disconnect = s_connect_started;
        esp_err_t err = bt_audio_cancel_connect();
        if (err != ESP_OK) {
            s_forget_status = BT_FORGET_FAILED;
            notify_bt_state();
            return err;
        }
    } else if (bda_equal(bda, s_peer_bda) &&
               (s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING)) {
        s_forget_wait_disconnect = true;
        esp_err_t err = bt_audio_disconnect();
        if (err != ESP_OK) {
            s_forget_status = BT_FORGET_FAILED;
            notify_bt_state();
            return err;
        }
    }
    return ESP_OK;
}

static void finish_forget(bool removed)
{
    if (removed) {
        esp_bd_addr_t saved;
        const app_settings_t *current = app_settings_get();
        if (str2bda(current->last_peer_addr, saved) && bda_equal(saved, s_forget_bda)) {
            app_settings_t cfg = *current;
            cfg.last_peer_addr[0] = '\0';
            cfg.last_peer_name[0] = '\0';
            if (app_settings_update(&cfg) != ESP_OK) {
                removed = false;
            }
        }
    }
    s_forget_wait_disconnect = false;
    s_forget_bond_requested = false;
    s_forget_deadline_ms = 0;
    s_forget_status = removed ? BT_FORGET_DONE : BT_FORGET_FAILED;
    ESP_LOGI(TAG, "forget peer %s", removed ? "done" : "failed");
    notify_bt_state();
}

static void advance_forget(uint32_t now)
{
    if (s_forget_status != BT_FORGET_PENDING) {
        return;
    }
    if (s_forget_wait_disconnect) {
        if ((int32_t)(now - s_forget_deadline_ms) >= 0) {
            finish_forget(false);
        }
        return;
    }
    if (s_forget_bond_requested) {
        if (s_forget_remove_result >= 0) {
            finish_forget(s_forget_remove_result == 0);
        } else if ((int32_t)(now - s_forget_deadline_ms) >= 0) {
            finish_forget(false);
        }
        return;
    }
    bool bonded = false;
    esp_err_t err = is_bonded(s_forget_bda, &bonded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "query bond failed: %s", esp_err_to_name(err));
        finish_forget(false);
    } else if (!bonded) {
        finish_forget(true);
    } else {
        s_forget_bond_requested = true;
        err = esp_bt_gap_remove_bond_device(s_forget_bda);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "remove bond failed to start: %s", esp_err_to_name(err));
            finish_forget(false);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 工作队列：把栈回调里不能做的事挪到任务上下文                        */
/* ------------------------------------------------------------------ */
static void bt_work_task(void *arg)
{
    (void)arg;
    uint16_t evt;
    uint8_t last_volume = app_settings_get()->volume;
    TickType_t volume_changed_at = 0;
    bool volume_pending_save = false;
    for (;;) {
        if (xQueueReceive(s_work_q, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (evt) {
            case WORK_REGISTER_CODECS: {
                if (s_state != BT_ST_INITING) break;
                esp_a2d_mcc_t aac_mcc, sbc_mcc;
                build_aac_mcc(&aac_mcc, ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC |
                                        ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC);
                build_sbc_mcc(&sbc_mcc, true);
                s_sep_ready_mask = 0;
                esp_err_t err;
                if (s_codec_pref == BT_CODEC_SBC) {
                    err = esp_a2d_source_register_stream_endpoint(0, &sbc_mcc);
                    if (err == ESP_OK) err = esp_a2d_source_register_stream_endpoint(1, &aac_mcc);
                } else {
                    err = esp_a2d_source_register_stream_endpoint(0, &aac_mcc);
                    if (err == ESP_OK) err = esp_a2d_source_register_stream_endpoint(1, &sbc_mcc);
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "A2DP codec registration request failed: %s", esp_err_to_name(err));
                    set_state(BT_ST_ERROR);
                }
                break;
            }
            case WORK_BT_READY: {
                if (s_state != BT_ST_INITING || s_sep_ready_mask != 3) break;
                esp_err_t err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                           ESP_BT_GENERAL_DISCOVERABLE);
                if (err != ESP_OK)
                    ESP_LOGW(TAG, "BT scan mode failed: %s", esp_err_to_name(err));
                char addr[18];
                bda2str((uint8_t *)esp_bt_dev_get_address(), addr, sizeof(addr));
                ESP_LOGI(TAG, "A2DP source codecs: AAC and SBC (pref=%u)", s_codec_pref);
                ESP_LOGI(TAG, "classic BT up: name=%s addr=%s", LOCAL_NAME, addr);
                set_state(BT_ST_READY);
                const app_settings_t *cfg = app_settings_get();
                if (cfg->auto_reconnect && cfg->last_peer_addr[0]) {
                    ESP_LOGI(TAG, "auto reconnect -> %s", cfg->last_peer_addr);
                    bt_audio_connect_addr(cfg->last_peer_addr);
                }
                break;
            }
            case WORK_APPLY_CODEC:
                if ((s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING) &&
                    s_conn_hdl && !s_cancel_requested) {
                    if (!s_fallback_sbc && s_codec_pref != BT_CODEC_SBC &&
                        s_audio_codec == ESP_A2D_MCT_M24) {
                        a2d_post(WORK_CHECK_READY);
                        break;
                    }
                    esp_a2d_mcc_t preferred;
                    if (s_codec_pref == BT_CODEC_SBC || s_fallback_sbc)
                        build_sbc_mcc(&preferred, false);
                    else {
                        uint8_t object_type =
                            (s_sink_aac_obj_types & ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC) ?
                            ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC :
                            ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC;
                        build_aac_mcc(&preferred, object_type);
                    }
                    s_pref_pending = true;
                    s_pref_deadline_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
                    esp_err_t pref_err = esp_a2d_source_set_pref_mcc(s_conn_hdl, &preferred);
                    if (pref_err != ESP_OK) {
                        ESP_LOGW(TAG, "codec preference request failed: %s; use negotiated codec",
                                 esp_err_to_name(pref_err));
                        s_pref_pending = false;
                        if (s_fallback_sbc) esp_a2d_source_disconnect(s_peer_bda);
                        else a2d_post(WORK_CHECK_READY);
                    }
                }
                break;
            case WORK_CHECK_READY:
                if ((s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING) &&
                    s_forget_status != BT_FORGET_PENDING && !s_cancel_requested &&
                    !s_pref_pending &&
                    (!s_fallback_sbc || s_audio_codec == ESP_A2D_MCT_SBC)) {
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
                }
                break;
            case WORK_STREAM_START:
                if (s_media_started && s_conn_hdl && s_audio_mtu &&
                    s_stream_mcc.type == s_audio_codec &&
                    (s_audio_codec == ESP_A2D_MCT_SBC || s_audio_codec == ESP_A2D_MCT_M24)) {
                    if (s_fallback_sbc && s_audio_codec != ESP_A2D_MCT_SBC) break;
                    esp_err_t stream_err = bt_stream_start(s_conn_hdl, s_audio_mtu,
                                                           &s_stream_mcc, s_stream_generation);
                    if (stream_err == ESP_OK && s_fallback_sbc)
                        s_fallback_deadline_ms = 0;
                    if (stream_err != ESP_OK)
                        ESP_LOGE(TAG, "audio stream start failed: %s", esp_err_to_name(stream_err));
                }
                break;
            case WORK_START_MEDIA:
                if ((s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING) &&
                    s_forget_status != BT_FORGET_PENDING && !s_cancel_requested &&
                    (!s_fallback_sbc || s_audio_codec == ESP_A2D_MCT_SBC)) {
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                }
                break;
            case WORK_SAVE_PEER:
                if ((s_state == BT_ST_CONNECTED || s_state == BT_ST_STREAMING) &&
                    s_forget_status != BT_FORGET_PENDING && !s_cancel_requested) {
                    app_settings_t cfg = *app_settings_get();
                    bool same_peer = strcmp(cfg.last_peer_addr, s_current_addr) == 0;
                    bda2str(s_peer_bda, cfg.last_peer_addr, sizeof(cfg.last_peer_addr));
                    if (usable_peer_name(s_peer_name, s_peer_bda))
                        snprintf(cfg.last_peer_name, sizeof(cfg.last_peer_name), "%s", s_peer_name);
                    else if (!same_peer || !usable_peer_name(cfg.last_peer_name, s_peer_bda))
                        cfg.last_peer_name[0] = '\0';
                    app_settings_update(&cfg);
                }
                break;
            case WORK_AVRC_PLAY:
                if (player_get_state() == PLAYER_PAUSED) {
                    player_resume();
                } else if (player_get_state() == PLAYER_STOPPED ||
                           player_get_state() == PLAYER_ERROR) {
                    player_toggle();
                }
                break;
            case WORK_AVRC_PAUSE:
                player_pause();
                break;
            case WORK_AVRC_NEXT: {
                esp_err_t err = player_next();
                ESP_LOGI(TAG, "headset next result=%s stack free=%u",
                         esp_err_to_name(err), (unsigned)uxTaskGetStackHighWaterMark(NULL));
                break;
            }
            case WORK_AVRC_PREV: {
                esp_err_t err = player_prev();
                ESP_LOGI(TAG, "headset prev result=%s stack free=%u",
                         esp_err_to_name(err), (unsigned)uxTaskGetStackHighWaterMark(NULL));
                break;
            }
            case WORK_AVRC_VOL_UP:
            case WORK_AVRC_VOL_DOWN:
                app_settings_volume_step(evt == WORK_AVRC_VOL_UP ? 1 : -1, 4);
                bt_audio_set_absolute_volume(app_settings_get()->volume);
                break;
            default:
                break;
            }
        }
        uint32_t failed_generation;
        esp_a2d_conn_hdl_t failed_conn;
        uint8_t failed_codec;
        if (bt_stream_take_failure(&failed_generation, &failed_conn, &failed_codec) &&
            failed_generation == s_stream_generation && failed_conn == s_conn_hdl &&
            s_media_started && (s_state == BT_ST_STREAMING || s_state == BT_ST_CONNECTED)) {
            ESP_LOGE(TAG, "%s stream failed", failed_codec == ESP_A2D_MCT_M24 ? "AAC" : "SBC");
            s_media_started = false;
            set_state(BT_ST_CONNECTED);
            if (failed_codec == ESP_A2D_MCT_M24 && !s_fallback_sbc) {
                s_fallback_sbc = true;
                s_fallback_apply_queued = false;
                s_fallback_wait_codec = false;
                s_fallback_deadline_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 5000;
                esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "cannot suspend failed AAC stream: %s", esp_err_to_name(err));
                    esp_a2d_source_disconnect(s_peer_bda);
                }
            } else {
                esp_a2d_source_disconnect(s_peer_bda);
            }
        }
        uint8_t volume = app_settings_get()->volume;
        if (volume != last_volume) {
            last_volume = volume;
            volume_changed_at = xTaskGetTickCount();
            volume_pending_save = true;
        } else if (volume_pending_save &&
                   xTaskGetTickCount() - volume_changed_at >= pdMS_TO_TICKS(2000)) {
            if (app_settings_save() == ESP_OK) {
                volume_pending_save = false;
            } else {
                volume_changed_at = xTaskGetTickCount();
            }
        }
        /* 发现超时 */
        if (s_disc_active && s_disc_deadline_ms) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (now > s_disc_deadline_ms) {
                s_disc_deadline_ms = 0;
                esp_bt_gap_cancel_discovery();
            }
        }
        if (s_state == BT_ST_CONNECTING && s_connect_deadline_ms) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((int32_t)(now - s_connect_deadline_ms) >= 0) {
                ESP_LOGW(TAG, "A2DP connect timed out");
                s_connect_pending = false;
                s_connect_deadline_ms = 0;
                if (s_disc_active) {
                    esp_bt_gap_cancel_discovery();
                }
                if (s_connect_started) {
                    memcpy(s_reject_late_bda, s_peer_bda, ESP_BD_ADDR_LEN);
                    s_reject_late = true;
                    s_reject_late_deadline_ms = now + 60000;
                    esp_a2d_source_disconnect(s_peer_bda);
                }
                s_connect_started = false;
                s_current_addr[0] = '\0';
                s_peer_name[0] = '\0';
                set_state(BT_ST_READY);
            }
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_fallback_deadline_ms &&
            (int32_t)(now - s_fallback_deadline_ms) >= 0) {
            ESP_LOGE(TAG, "SBC fallback timed out; disconnecting");
            s_fallback_deadline_ms = 0;
            esp_a2d_source_disconnect(s_peer_bda);
        }
        if (s_pref_pending && (int32_t)(now - s_pref_deadline_ms) >= 0) {
            ESP_LOGW(TAG, "codec preference timed out; use negotiated codec");
            s_pref_pending = false;
            if (s_fallback_sbc) esp_a2d_source_disconnect(s_peer_bda);
            else a2d_post(WORK_CHECK_READY);
        }
        if (s_cancel_requested && (int32_t)(now - s_cancel_deadline_ms) >= 0) {
            ESP_LOGW(TAG, "A2DP cancel timed out");
            esp_a2d_source_disconnect(s_peer_bda);
            s_cancel_requested = false;
            s_connect_started = false;
            s_cancel_deadline_ms = 0;
            s_current_addr[0] = '\0';
            s_peer_name[0] = '\0';
            set_state(BT_ST_READY);
            if (s_forget_status == BT_FORGET_PENDING && s_forget_wait_disconnect) {
                finish_forget(false);
            }
        }
        if (s_reject_late && (int32_t)(now - s_reject_late_deadline_ms) >= 0) {
            s_reject_late = false;
        }
        advance_forget(now);
    }
}

/* ------------------------------------------------------------------ */
/* 初始�?                                                             */
/* ------------------------------------------------------------------ */
esp_err_t bt_audio_init(void)
{
    set_state(BT_ST_INITING);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((err = esp_bluedroid_init_with_cfg(&bd_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }
    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    esp_bt_gap_set_device_name(LOCAL_NAME);
    esp_bt_gap_register_callback(gap_cb);

    if ((err = esp_avrc_ct_register_callback(avrc_ct_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "avrc ct callback failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }
    if ((err = esp_avrc_ct_init()) != ESP_OK) {
        ESP_LOGE(TAG, "avrc ct init failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }
    if ((err = esp_avrc_tg_register_callback(avrc_tg_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "avrc tg callback failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }
    if ((err = esp_avrc_tg_init()) != ESP_OK) {
        ESP_LOGE(TAG, "avrc tg init failed: %s", esp_err_to_name(err));
        set_state(BT_ST_ERROR);
        return err;
    }

    s_work_q = xQueueCreate(16, sizeof(uint16_t));
    if (!s_work_q || xTaskCreate(bt_work_task, "bt_work", 16384, NULL, 8,
                                 &s_work_task) != pdPASS) {
        if (s_work_q) vQueueDelete(s_work_q);
        s_work_q = NULL;
        ESP_LOGE(TAG, "BT work task allocation failed");
        set_state(BT_ST_ERROR);
        return ESP_ERR_NO_MEM;
    }

    err = esp_a2d_register_callback(a2d_cb);
    if (err == ESP_OK) err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source setup failed: %s", esp_err_to_name(err));
        vTaskDelete(s_work_task);
        s_work_task = NULL;
        vQueueDelete(s_work_q);
        s_work_q = NULL;
        set_state(BT_ST_ERROR);
        return err;
    }
    return ESP_OK;
}


/* ------------------------------------------------------------------ */
/* 编解码器偏好                                                        */
/* ------------------------------------------------------------------ */
esp_err_t bt_audio_set_codec_pref(uint8_t pref)
{
    if (pref > BT_CODEC_AAC) {
        return ESP_ERR_INVALID_ARG;
    }
    s_codec_pref = pref;
    ESP_LOGI(TAG, "A2DP codec preference=%u (next connection)", pref);
    return ESP_OK;
}

uint8_t bt_audio_get_codec_pref(void)
{
    return s_codec_pref;
}

const char *bt_audio_active_codec_name(void)
{
    if (s_state != BT_ST_CONNECTED && s_state != BT_ST_STREAMING) {
        return "-";
    }
    switch (s_audio_codec) {
    case ESP_A2D_MCT_SBC:
        return "SBC";
    case ESP_A2D_MCT_M24:
        return "AAC";
    default:
        return "?";
    }
}

#include "bt_stream.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "aac_enc.h"
#include "esp_audio_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sbc_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music.h"

#define PCM_RATE 44100
#define PCM_CHANNELS 2
#define AAC_BITRATE_MAX 160000
#define AAC_MTU_RESERVE 20
#define SBC_MTU_RESERVE 1
#define SBC_FRAMES_PER_PACKET 4
#define AAC_LATE_LIMIT 8
#define BT_STREAM_TASK_PRIORITY 8
#define BT_STREAM_CORE 1

static const char *TAG = "bt_stream";
static struct {
    esp_a2d_conn_hdl_t conn;
    uint16_t mtu;
    esp_a2d_mcc_t mcc;
    uint32_t generation;
} s_config;
static _Atomic(TaskHandle_t) s_task;
static atomic_bool s_run;
static bool s_sbc_registered;
static _Atomic uint32_t s_send_errors;
static _Atomic uint32_t s_send_retries;
static _Atomic uint32_t s_send_drops;
static _Atomic uint32_t s_late_resets;
static _Atomic uint32_t s_max_encode_us;
static _Atomic uint32_t s_max_send_us;
static _Atomic uint32_t s_max_work_us;
static portMUX_TYPE s_failure_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t generation;
    esp_a2d_conn_hdl_t conn;
    uint8_t codec;
    bool pending;
} s_failure;

static int aac_bitrate_for_mtu(uint16_t mtu)
{
    if (mtu <= AAC_MTU_RESERVE) return 0;
    /* AAC frame sizes vary; keep peak headroom for a complete frame in one packet. */
    uint64_t limit = (uint64_t)(mtu - AAC_MTU_RESERVE) * PCM_RATE * 8 * 10 / (1024 * 18);
    int bitrate = (int)(limit / 8000 * 8000);
    if (bitrate > AAC_BITRATE_MAX) bitrate = AAC_BITRATE_MAX;
    return bitrate >= 64000 ? bitrate : 0;
}

static void stat_max(_Atomic uint32_t *value, uint32_t sample)
{
    uint32_t old = atomic_load(value);
    while (old < sample &&
           !atomic_compare_exchange_weak(value, &old, sample)) {
    }
}

static esp_sbc_ch_mode_t sbc_channel_mode(uint8_t mask)
{
    if (mask & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO) return ESP_SBC_CH_MODE_JOINT_STEREO;
    if (mask & ESP_A2D_SBC_CIE_CH_MODE_STEREO) return ESP_SBC_CH_MODE_STEREO;
    return ESP_SBC_CH_MODE_DUAL;
}

static esp_audio_enc_handle_t sbc_open(const esp_a2d_cie_sbc_t *cie,
                                       int *in_bytes, int *out_bytes)
{
    if (!s_sbc_registered) {
        if (esp_sbc_enc_register() != ESP_AUDIO_ERR_OK) return NULL;
        s_sbc_registered = true;
    }

    esp_sbc_enc_config_t cfg = ESP_SBC_STD_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = PCM_RATE;
    cfg.ch_mode = sbc_channel_mode(cie->ch_mode);
    cfg.block_length = (cie->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_16) ? 16 :
                       (cie->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_12) ? 12 :
                       (cie->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_8) ? 8 : 4;
    cfg.sub_bands_num = (cie->num_subbands & ESP_A2D_SBC_CIE_NUM_SUBBANDS_8) ? 8 : 4;
    cfg.allocation_method = (cie->alloc_mthd & ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS) ?
                            ESP_SBC_ALLOC_LOUDNESS : ESP_SBC_ALLOC_SNR;
    cfg.bitpool = cie->max_bitpool;
    if (cfg.bitpool > 53) cfg.bitpool = 53;
    if (cfg.bitpool < cie->min_bitpool) cfg.bitpool = cie->min_bitpool;

    esp_audio_enc_config_t enc_cfg = {
        .type = ESP_AUDIO_TYPE_SBC,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    esp_audio_enc_handle_t encoder = NULL;
    if (esp_audio_enc_open(&enc_cfg, &encoder) != ESP_AUDIO_ERR_OK) return NULL;
    if (esp_audio_enc_get_frame_size(encoder, in_bytes, out_bytes) != ESP_AUDIO_ERR_OK ||
        *in_bytes <= 0 || *out_bytes <= 0) {
        esp_audio_enc_close(encoder);
        return NULL;
    }
    ESP_LOGI(TAG, "SBC 44.1k ch=%d blocks=%u bands=%u bitpool=%u frame=%d/%d",
             cfg.ch_mode, cfg.block_length, cfg.sub_bands_num, cfg.bitpool,
             *in_bytes, *out_bytes);
    return encoder;
}

static esp_err_t send_packet(const uint8_t *data, int len, int frames, uint32_t timestamp)
{
    esp_a2d_audio_buff_t *buff = esp_a2d_audio_buff_alloc((uint16_t)len);
    if (!buff) return ESP_ERR_NO_MEM;
    memcpy(buff->data, data, (size_t)len);
    buff->data_len = (uint16_t)len;
    buff->number_frame = (uint16_t)frames;
    buff->timestamp = timestamp;

    esp_err_t err = ESP_FAIL;
    for (int retry = 0; retry < 6 && atomic_load(&s_run); retry++) {
        err = esp_a2d_source_audio_data_send(s_config.conn, buff);
        if (err == ESP_OK) {
            if (retry) atomic_fetch_add(&s_send_retries, (uint32_t)retry);
            return ESP_OK;  /* Bluedroid now owns the buffer. */
        }
        atomic_fetch_add(&s_send_errors, 1);
        if (err != ESP_FAIL) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    atomic_fetch_add(&s_send_drops, 1);
    esp_a2d_audio_buff_free(buff);
    return err;
}

static bool pace(int64_t *next_us, uint32_t sample_count)
{
    *next_us += (int64_t)sample_count * 1000000 / PCM_RATE;
    int64_t now = esp_timer_get_time();
    bool late = false;
    if (*next_us < now - 50000) {
        atomic_fetch_add(&s_late_resets, 1);
        late = true;
        *next_us = now;
    }
    while (atomic_load(&s_run) && now < *next_us) {
        int delay_ms = (int)((*next_us - now) / 1000);
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
        now = esp_timer_get_time();
    }
    return late;
}

static void stream_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* Start only after s_task is published. */
    bool aac = s_config.mcc.type == ESP_A2D_MCT_M24;
    aac_enc_t *aac_encoder = NULL;
    esp_audio_enc_handle_t sbc_encoder = NULL;
    uint8_t *pcm = NULL;
    uint8_t *encoded = NULL;
    int in_bytes = 0, out_bytes = 0;
    int packet_frames = 1;
    int consecutive_errors = 0;
    bool failed = false;
    uint32_t last_health_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t late_window_ms = last_health_ms;
    unsigned late_window_count = 0;

    if (!atomic_load(&s_run)) goto cleanup;

    if (aac) {
        int bitrate = aac_bitrate_for_mtu(s_config.mtu);
        if (!bitrate) ESP_LOGE(TAG, "MTU %u too small for AAC", s_config.mtu);
        else aac_encoder = aac_enc_open(PCM_RATE, PCM_CHANNELS, bitrate);
        if (aac_encoder) {
            in_bytes = aac_enc_frame_samples(aac_encoder) * PCM_CHANNELS * sizeof(int16_t);
            out_bytes = aac_enc_max_output(aac_encoder);
        }
    } else {
        sbc_encoder = sbc_open(&s_config.mcc.cie.sbc_info, &in_bytes, &out_bytes);
        if (sbc_encoder) {
            packet_frames = (s_config.mtu - SBC_MTU_RESERVE) / out_bytes;
            if (packet_frames < 1) packet_frames = 1;
            if (packet_frames > SBC_FRAMES_PER_PACKET) packet_frames = SBC_FRAMES_PER_PACKET;
        }
    }
    if (in_bytes <= 0 || out_bytes <= 0) {
        failed = true;
        goto cleanup;
    }
    if (s_config.mtu <= (aac ? AAC_MTU_RESERVE : SBC_MTU_RESERVE) ||
        (!aac && out_bytes > s_config.mtu - SBC_MTU_RESERVE)) {
        ESP_LOGE(TAG, "MTU %u cannot carry a %s frame (buffer %d)",
                 s_config.mtu, aac ? "AAC" : "SBC", out_bytes);
        failed = true;
        goto cleanup;
    }

    pcm = heap_caps_malloc((size_t)in_bytes * packet_frames,
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    encoded = malloc((size_t)out_bytes * packet_frames);
    if (!pcm || !encoded) {
        ESP_LOGE(TAG, "stream buffer allocation failed");
        failed = true;
        goto cleanup;
    }

    uint32_t sample_count = (uint32_t)in_bytes / (PCM_CHANNELS * sizeof(int16_t));
    uint32_t timestamp = 0;
    int64_t next_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s stream start, mtu=%u packet_frames=%d", aac ? "AAC" : "SBC",
             s_config.mtu, packet_frames);

    while (atomic_load(&s_run)) {
        int64_t work_start_us = esp_timer_get_time();
        int payload_len = 0;
        int frames = 0;
        player_read_pcm(pcm, in_bytes * packet_frames);
        int64_t encode_start_us = esp_timer_get_time();
        for (int i = 0; i < packet_frames && atomic_load(&s_run); i++) {
            uint8_t *dst = encoded + payload_len;
            int len = -1;
            if (aac) {
                len = aac_enc_encode(aac_encoder, (int16_t *)(pcm + i * in_bytes),
                                     (int)sample_count, dst, out_bytes);
            } else {
                esp_audio_enc_in_frame_t input = {
                    .buffer = pcm + i * in_bytes, .len = (uint32_t)in_bytes,
                };
                esp_audio_enc_out_frame_t output = {
                    .buffer = dst, .len = (uint32_t)out_bytes,
                };
                if (esp_audio_enc_process(sbc_encoder, &input, &output) == ESP_AUDIO_ERR_OK)
                    len = (int)output.encoded_bytes;
            }
            if (len <= 0 || len > out_bytes) break;
            payload_len += len;
            frames++;
        }
        stat_max(&s_max_encode_us,
                 (uint32_t)(esp_timer_get_time() - encode_start_us));
        if (frames == packet_frames) {
            int max_payload = s_config.mtu - (aac ? AAC_MTU_RESERVE : SBC_MTU_RESERVE);
            if (payload_len <= max_payload) {
                int64_t send_start_us = esp_timer_get_time();
                esp_err_t err = send_packet(encoded, payload_len, frames, timestamp);
                stat_max(&s_max_send_us,
                         (uint32_t)(esp_timer_get_time() - send_start_us));
                if (err == ESP_ERR_INVALID_STATE) {
                    failed = true;
                    break;
                }
                if (err == ESP_OK) {
                    consecutive_errors = 0;
                } else {
                    if (err != ESP_FAIL)
                        ESP_LOGW(TAG, "audio send failed: %s", esp_err_to_name(err));
                    consecutive_errors++;
                }
            } else {
                ESP_LOGW(TAG, "encoded packet %d exceeds MTU %u", payload_len, s_config.mtu);
                consecutive_errors++;
            }
        } else {
            consecutive_errors++;
        }
        if (consecutive_errors >= 8) {
            ESP_LOGE(TAG, "too many consecutive %s stream errors", aac ? "AAC" : "SBC");
            failed = true;
            break;
        }
        stat_max(&s_max_work_us,
                 (uint32_t)(esp_timer_get_time() - work_start_us));
        uint32_t health_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (health_ms - last_health_ms >= 5000) {
            last_health_ms = health_ms;
            ESP_LOGI(TAG,
                     "%s health: send_err=%lu drops=%lu retries=%lu late=%lu "
                     "max_encode=%lu ms max_send=%lu ms max_work=%lu ms buffer=%u%%",
                     aac ? "AAC" : "SBC",
                     (unsigned long)atomic_exchange(&s_send_errors, 0),
                     (unsigned long)atomic_exchange(&s_send_drops, 0),
                     (unsigned long)atomic_exchange(&s_send_retries, 0),
                     (unsigned long)atomic_exchange(&s_late_resets, 0),
                     (unsigned long)(atomic_exchange(&s_max_encode_us, 0) / 1000),
                     (unsigned long)(atomic_exchange(&s_max_send_us, 0) / 1000),
                     (unsigned long)(atomic_exchange(&s_max_work_us, 0) / 1000),
                     player_buffer_level());
        }
        timestamp += sample_count * packet_frames;
        if (pace(&next_us, sample_count * packet_frames)) late_window_count++;
        uint32_t window_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (window_ms - late_window_ms >= 2000) {
            if (aac && player_get_state() == PLAYER_PLAYING &&
                late_window_count >= AAC_LATE_LIMIT) {
                ESP_LOGW(TAG, "AAC cannot keep real time (%u late frames/2s); switching to SBC",
                         late_window_count);
                failed = true;
                break;
            }
            late_window_count = 0;
            late_window_ms = window_ms;
        }
    }

cleanup:
    uint32_t generation = s_config.generation;
    esp_a2d_conn_hdl_t conn = s_config.conn;
    uint8_t codec = s_config.mcc.type;
    free(pcm);
    free(encoded);
    if (aac_encoder) aac_enc_close(aac_encoder);
    if (sbc_encoder) esp_audio_enc_close(sbc_encoder);
    bool was_running = atomic_exchange(&s_run, false);
    atomic_store(&s_task, NULL);
    if (failed && was_running) {
        portENTER_CRITICAL(&s_failure_lock);
        s_failure.generation = generation;
        s_failure.conn = conn;
        s_failure.codec = codec;
        s_failure.pending = true;
        portEXIT_CRITICAL(&s_failure_lock);
    }
    ESP_LOGI(TAG, "stream stopped");
    vTaskDelete(NULL);
}

bool bt_stream_take_failure(uint32_t *generation, esp_a2d_conn_hdl_t *conn, uint8_t *codec)
{
    if (!generation || !conn || !codec) return false;
    portENTER_CRITICAL(&s_failure_lock);
    bool pending = s_failure.pending;
    if (pending) {
        *generation = s_failure.generation;
        *conn = s_failure.conn;
        *codec = s_failure.codec;
        s_failure.pending = false;
    }
    portEXIT_CRITICAL(&s_failure_lock);
    return pending;
}

esp_err_t bt_stream_start(esp_a2d_conn_hdl_t conn, uint16_t mtu, const esp_a2d_mcc_t *mcc,
                          uint32_t generation)
{
    if (!conn || !mtu || !mcc || (mcc->type != ESP_A2D_MCT_SBC && mcc->type != ESP_A2D_MCT_M24))
        return ESP_ERR_INVALID_ARG;

    bool same_codec = s_config.mcc.type == mcc->type &&
        (mcc->type == ESP_A2D_MCT_SBC ?
         memcmp(&s_config.mcc.cie.sbc_info, &mcc->cie.sbc_info,
                sizeof(mcc->cie.sbc_info)) == 0 :
         memcmp(&s_config.mcc.cie.m24_info, &mcc->cie.m24_info,
                sizeof(mcc->cie.m24_info)) == 0);
    if (atomic_load(&s_task) && atomic_load(&s_run) &&
        s_config.conn == conn && s_config.mtu == mtu &&
        s_config.generation == generation && same_codec) {
        return ESP_OK;
    }

    bt_stream_stop();
    for (int i = 0; atomic_load(&s_task) && i < 100; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (atomic_load(&s_task)) return ESP_ERR_INVALID_STATE;

    s_config.conn = conn;
    s_config.mtu = mtu;
    s_config.mcc = *mcc;
    s_config.generation = generation;
    atomic_store(&s_send_errors, 0);
    atomic_store(&s_send_retries, 0);
    atomic_store(&s_send_drops, 0);
    atomic_store(&s_late_resets, 0);
    atomic_store(&s_max_encode_us, 0);
    atomic_store(&s_max_send_us, 0);
    atomic_store(&s_max_work_us, 0);
    atomic_store(&s_run, true);
    TaskHandle_t task = NULL;
#if CONFIG_FREERTOS_UNICORE
    BaseType_t task_ok = xTaskCreate(stream_task, "bt_encode", 8192, NULL,
                                     BT_STREAM_TASK_PRIORITY, &task);
#else
    BaseType_t task_ok = xTaskCreatePinnedToCore(stream_task, "bt_encode", 8192,
                                                 NULL, BT_STREAM_TASK_PRIORITY,
                                                 &task, BT_STREAM_CORE);
#endif
    if (task_ok != pdPASS) {
        atomic_store(&s_run, false);
        atomic_store(&s_task, NULL);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_task, task);
    xTaskNotifyGive(task);
    return ESP_OK;
}

void bt_stream_stop(void)
{
    atomic_store(&s_run, false);
}

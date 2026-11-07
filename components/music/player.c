#include "music.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_core.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "decoder.h"

static const char *TAG = "player";

#define PCM_RATE 44100
#define PCM_CH 2
#define OUT_BYTES_PER_SEC (PCM_RATE * PCM_CH * 2)
#define RING_BYTES (768 * 1024) /* ~4.3 秒 @44.1k/16bit/stereo，放 PSRAM */
#define MAX_PCM_FRAMES 4096     /* FLAC 一帧最大 4096 样本，MP3 是 1152 */
#define PREFILL_BYTES (192 * 1024) /* 起播前预灌 1.1 秒，避免欠载 */
#define DECODE_RESERVE_BYTES (MAX_PCM_FRAMES * 2 * PCM_CH * sizeof(int16_t))
#define INITIAL_DECODE_RETRIES 3

static uint8_t *s_ring;
static size_t s_head; /* 写入位置 */
static size_t s_tail; /* 读取位置 */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_decoder_lock;

static audio_dec_t *s_dec;
static audio_info_t s_info;

static volatile player_state_t s_state = PLAYER_STOPPED;
static int s_index = -1;
static bool s_auto_next = true;
static int *s_collection_indices;
static int s_collection_count;
static uint32_t s_collection_generation;
static uint64_t s_consumed_bytes;
static portMUX_TYPE s_progress_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_pos_ms;
static uint32_t s_underrun_bytes;
static uint32_t s_dur_ms;
static volatile bool s_seek_pending;
static uint32_t s_seek_ms;
static bool s_eof_pending;
static bool s_decoded_any_pcm;
static uint8_t s_initial_empty_reads;

static music_item_t s_current;
static bool s_have_current;

static uint8_t s_bands[10];
static uint8_t s_peak;
static TaskHandle_t s_task;

static int do_resample(const int16_t *in, int frames_in, int16_t *out);

static int16_t *s_pcm;     /* 解码输出缓冲 */
static int16_t *s_resamp;  /* 重采样缓冲 */
static float s_lp[4];

/* ------------------------------------------------------------------ */
/* 环形缓冲                                                            */
/* ------------------------------------------------------------------ */
static size_t ring_used(void)
{
    size_t v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = (s_head + RING_BYTES - s_tail) % RING_BYTES;
    xSemaphoreGive(s_lock);
    return v;
}

static size_t ring_free(void)
{
    return RING_BYTES - 1 - ring_used();
}

static size_t ring_write(const void *src, size_t len)
{
    const uint8_t *p = src;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t free_sz = RING_BYTES - 1 - ((s_head + RING_BYTES - s_tail) % RING_BYTES);
    if (len > free_sz) {
        len = free_sz;
    }
    size_t first = RING_BYTES - s_head;
    if (len < first) {
        first = len;
    }
    memcpy(s_ring + s_head, p, first);
    if (len > first) {
        memcpy(s_ring, p + first, len - first);
    }
    s_head = (s_head + len) % RING_BYTES;
    xSemaphoreGive(s_lock);
    return len;
}

static size_t ring_read(void *dst, size_t len)
{
    uint8_t *d = dst;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t used = (s_head + RING_BYTES - s_tail) % RING_BYTES;
    if (len > used) {
        len = used;
    }
    size_t first = RING_BYTES - s_tail;
    if (len < first) {
        first = len;
    }
    memcpy(d, s_ring + s_tail, first);
    if (len > first) {
        memcpy(d + first, s_ring, len - first);
    }
    s_tail = (s_tail + len) % RING_BYTES;
    xSemaphoreGive(s_lock);
    return len;
}

/* ------------------------------------------------------------------ */
/* 可视化分析（一阶滤波器组，近似频谱）                                */
/* ------------------------------------------------------------------ */
static void analyze_pcm(const int16_t *pcm, int frames)
{
    if (frames <= 0) {
        return;
    }
    float acc[5] = {0};
    int step = frames > 256 ? frames / 256 : 1;
    int n = 0;
    for (int i = 0; i < frames; i += step) {
        float x = ((float)pcm[i * 2] + (float)pcm[i * 2 + 1]) * (1.0f / 65536.0f);
        s_lp[0] += (x - s_lp[0]) * 0.05f;
        s_lp[1] += (x - s_lp[1]) * 0.18f;
        s_lp[2] += (x - s_lp[2]) * 0.45f;
        s_lp[3] += (x - s_lp[3]) * 0.75f;
        acc[0] += fabsf(s_lp[0]);
        acc[1] += fabsf(s_lp[1] - s_lp[0]);
        acc[2] += fabsf(s_lp[2] - s_lp[1]);
        acc[3] += fabsf(s_lp[3] - s_lp[2]);
        acc[4] += fabsf(x - s_lp[3]);
        n++;
    }
    if (n == 0) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        acc[i] /= (float)n;
    }
    /* 5 段映射到 10 段显示（插值），做攻击/释放平滑 */
    for (int b = 0; b < 10; b++) {
        float pos = (float)b * 4.0f / 9.0f;
        int i0 = (int)pos;
        int i1 = i0 + 1 < 5 ? i0 + 1 : i0;
        float v = acc[i0] + (acc[i1] - acc[i0]) * (pos - (float)i0);
        int val = (int)(sqrtf(v * 380.0f) * 100.0f);
        if (val > 100) {
            val = 100;
        }
        uint8_t target = (uint8_t)val;
        if (target > s_bands[b]) {
            s_bands[b] = (uint8_t)((s_bands[b] + target * 3) / 4);
        } else {
            s_bands[b] = (uint8_t)((s_bands[b] * 7 + target) / 8);
        }
    }
    uint8_t mx = 0;
    for (int b = 0; b < 10; b++) {
        if (s_bands[b] > mx) {
            mx = s_bands[b];
        }
    }
    uint8_t pk = s_peak > 2 ? (uint8_t)(s_peak - 2) : 0;
    s_peak = mx > pk ? mx : pk;
}

uint8_t player_get_bands(uint8_t *bands, uint8_t max_bands)
{
    uint8_t n = max_bands > 10 ? 10 : max_bands;
    for (uint8_t i = 0; i < n; i++) {
        bands[i] = s_bands[i];
    }
    return n;
}

uint8_t player_get_peak(void)
{
    return s_peak;
}

uint8_t player_buffer_level(void)
{
    return (uint8_t)(ring_used() * 100 / RING_BYTES);
}

/* ------------------------------------------------------------------ */
/* 状态与事件                                                          */
/* ------------------------------------------------------------------ */
static void set_state(player_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    app_evt_t evt = {.id = APP_EVT_PLAYER_STATE};
    evt.player.state = (int)st;
    app_evt_post(&evt, 0);
}

const char *player_state_name(player_state_t st)
{
    switch (st) {
    case PLAYER_STOPPED:
        return "stopped";
    case PLAYER_LOADING:
        return "loading";
    case PLAYER_PLAYING:
        return "playing";
    case PLAYER_PAUSED:
        return "paused";
    default:
        return "error";
    }
}

player_state_t player_get_state(void)
{
    return s_state;
}

const music_item_t *player_get_current(void)
{
    return s_have_current ? &s_current : NULL;
}

uint32_t player_get_pos_ms(void)
{
    return s_pos_ms;
}

uint32_t player_get_dur_ms(void)
{
    return s_dur_ms;
}

/* ------------------------------------------------------------------ */
/* 打开 / 切歌                                                         */
/* ------------------------------------------------------------------ */
static void close_decoder(void)
{
    if (s_dec) {
        audio_dec_close(s_dec);
        s_dec = NULL;
    }
}

/* Caller holds s_decoder_lock so EOF advance can reopen without relocking. */
static esp_err_t open_index_locked(int index, bool auto_next)
{
    int n = music_lib_count();
    if (n <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (index < 0) {
        index = n - 1;
    }
    if (index >= n) {
        index = 0;
    }
    const music_item_t *it = music_lib_get(index);
    if (!it) {
        return ESP_ERR_NOT_FOUND;
    }

    set_state(PLAYER_LOADING);
    close_decoder();

    audio_dec_t *dec = audio_dec_open(it->path, it->fmt);
    if (!dec) {
        set_state(PLAYER_ERROR);
        return ESP_FAIL;
    }

    s_dec = dec;
    s_info = dec->info;
    s_index = index;
    s_auto_next = auto_next;
    s_current = *it;
    s_current.duration_ms = dec->info.duration_ms;
    s_dur_ms = s_current.duration_ms;
    s_have_current = true;
    s_seek_pending = false;
    s_eof_pending = false;
    s_decoded_any_pcm = false;
    s_initial_empty_reads = 0;
    portENTER_CRITICAL(&s_progress_mux);
    s_consumed_bytes = 0;
    portEXIT_CRITICAL(&s_progress_mux);
    s_pos_ms = 0;
    s_peak = 0;
    memset(s_bands, 0, sizeof(s_bands));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_head = s_tail = 0;
    xSemaphoreGive(s_lock);

    app_evt_t evt = {.id = APP_EVT_TRACK_CHANGED};
    evt.track.index = index;
    evt.track.total = n;
    evt.track.auto_advance = auto_next;
    app_evt_post(&evt, 0);

    ESP_LOGI(TAG, "open #%d %s [%s %dHz/%db] %lu ms, buffered %u KB", index, s_current.title,
             audio_dec_format_name(dec->info.fmt), dec->info.samplerate, dec->info.bits,
             (unsigned long)s_dur_ms, (unsigned)(ring_used() / 1024));
    return ESP_OK;
}

static esp_err_t open_index(int index, bool auto_next)
{
    if (music_lib_scanning() || music_update_mode()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_lib_scanning() || music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    free(s_collection_indices);
    s_collection_indices = NULL;
    s_collection_count = 0;
    esp_err_t err = open_index_locked(index, auto_next);
    xSemaphoreGive(s_decoder_lock);
    return err;
}

static int current_library_index(void)
{
    if (!s_have_current) {
        return -1;
    }
    int count = music_lib_count();
    for (int i = 0; i < count; i++) {
        const music_item_t *it = music_lib_get(i);
        if (it && strcmp(it->path, s_current.path) == 0) {
            return i;
        }
    }
    return s_index >= 0 && s_index < count ? s_index : -1;
}

static int collection_position(int current)
{
    if (!s_collection_indices || music_lib_scanning() ||
        s_collection_generation != music_lib_generation()) return -1;
    for (int i = 0; i < s_collection_count; i++) {
        if (s_collection_indices[i] == current) return i;
    }
    return -1;
}

static int next_library_index(int current, int count, bool automatic)
{
    const app_settings_t *cfg = app_settings_get();
    if (automatic && !cfg->shuffle && cfg->repeat == 2 && current >= 0) {
        return current; /* 单曲循环只影响播完后的自动切换 */
    }
    int position = collection_position(current);
    if (position >= 0) {
        if (cfg->shuffle && s_collection_count > 1) {
            int pick = (int)(esp_random() % (uint32_t)(s_collection_count - 1));
            if (pick >= position) pick++;
            return s_collection_indices[pick];
        }
        return s_collection_indices[(position + 1) % s_collection_count];
    }
    if (cfg->shuffle && count > 1) {
        /* 排除当前曲目，避免随机模式连续播同一首。 */
        int pick = (int)(esp_random() % (uint32_t)(current >= 0 ? count - 1 : count));
        return current >= 0 && pick >= current ? pick + 1 : pick;
    }
    return (current + 1) % count;
}

esp_err_t player_start_index(int index, bool auto_advance)
{
    return open_index(index, auto_advance);
}

esp_err_t player_start_collection(int index, const int *indices, int count, bool auto_advance)
{
    if (!indices || count <= 0 || count > music_lib_count() || music_lib_scanning() || music_update_mode())
        return ESP_ERR_INVALID_ARG;
    uint32_t generation = music_lib_generation();
    int *copy = heap_caps_malloc((size_t)count * sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = malloc((size_t)count * sizeof(int));
    if (!copy) return ESP_ERR_NO_MEM;
    bool includes_current = false;
    int total = music_lib_count();
    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= total) {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (indices[j] == indices[i]) {
                free(copy);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (indices[i] == index) includes_current = true;
        copy[i] = indices[i];
    }
    if (!includes_current) {
        free(copy);
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_lib_scanning() || music_update_mode() || generation != music_lib_generation()) {
        xSemaphoreGive(s_decoder_lock);
        free(copy);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = open_index_locked(index, auto_advance);
    if (err == ESP_OK) {
        free(s_collection_indices);
        s_collection_indices = copy;
        s_collection_count = count;
        s_collection_generation = generation;
    } else free(copy);
    xSemaphoreGive(s_decoder_lock);
    return err;
}

esp_err_t player_toggle(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (s_state == PLAYER_PLAYING || s_state == PLAYER_LOADING) {
        set_state(PLAYER_PAUSED);
    } else if (s_state == PLAYER_PAUSED) {
        set_state(ring_used() >= PREFILL_BYTES ? PLAYER_PLAYING : PLAYER_LOADING);
    } else {
        err = music_lib_scanning() ? ESP_ERR_INVALID_STATE :
              open_index_locked(s_have_current ? current_library_index() : 0, true);
    }
    xSemaphoreGive(s_decoder_lock);
    return err;
}

esp_err_t player_pause(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (s_state == PLAYER_PLAYING || s_state == PLAYER_LOADING) {
        set_state(PLAYER_PAUSED);
    }
    xSemaphoreGive(s_decoder_lock);
    return ESP_OK;
}

esp_err_t player_resume(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == PLAYER_PAUSED) {
        set_state(ring_used() >= PREFILL_BYTES ? PLAYER_PLAYING : PLAYER_LOADING);
    }
    xSemaphoreGive(s_decoder_lock);
    return ESP_OK;
}

esp_err_t player_stop(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    set_state(PLAYER_STOPPED);
    close_decoder();
    s_have_current = false;
    s_index = -1;
    s_dur_ms = 0;
    s_seek_pending = false;
    s_eof_pending = false;
    s_decoded_any_pcm = false;
    s_initial_empty_reads = 0;
    s_pos_ms = 0;
    portENTER_CRITICAL(&s_progress_mux);
    s_consumed_bytes = 0;
    portEXIT_CRITICAL(&s_progress_mux);
    free(s_collection_indices);
    s_collection_indices = NULL;
    s_collection_count = 0;
    xSemaphoreGive(s_decoder_lock);
    return ESP_OK;
}

esp_err_t player_next(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_lib_scanning() || music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int n = music_lib_count();
    if (n <= 0) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = open_index_locked(next_library_index(current_library_index(), n, false), s_auto_next);
    xSemaphoreGive(s_decoder_lock);
    return err;
}

esp_err_t player_prev(void)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_lib_scanning() || music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int n = music_lib_count();
    if (n <= 0) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_NOT_FOUND;
    }
    int index = current_library_index();
    int position = collection_position(index);
    int previous = position >= 0 ?
                   s_collection_indices[(position - 1 + s_collection_count) % s_collection_count] :
                   index < 0 ? n - 1 : (index - 1 + n) % n;
    esp_err_t err = open_index_locked(previous, s_auto_next);
    xSemaphoreGive(s_decoder_lock);
    return err;
}

esp_err_t player_seek_ms(uint32_t ms)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    if (music_update_mode()) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_dec) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dec->info.fmt == MUSIC_FMT_AAC || s_dec->info.fmt == MUSIC_FMT_M4A) {
        xSemaphoreGive(s_decoder_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_seek_ms = ms;
    s_seek_pending = true;
    xSemaphoreGive(s_decoder_lock);
    return ESP_OK;
}

bool player_can_seek(void)
{
    const music_item_t *item = player_get_current();
    return item && item->fmt != MUSIC_FMT_AAC && item->fmt != MUSIC_FMT_M4A;
}

/* ------------------------------------------------------------------ */
/* A2DP 数据回调                                                       */
/* ------------------------------------------------------------------ */
int32_t player_read_pcm(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        return 0;
    }
    if (s_state != PLAYER_PLAYING) {
        memset(buf, 0, (size_t)len);
        return len;
    }
    size_t got = ring_read(buf, (size_t)len);
    if (got < (size_t)len) {
        memset(buf + got, 0, (size_t)len - got);
        __atomic_fetch_add(&s_underrun_bytes, (uint32_t)((size_t)len - got),
                           __ATOMIC_RELAXED);
    }
    if (got > 0) {
        portENTER_CRITICAL(&s_progress_mux);
        s_consumed_bytes += got;
        s_pos_ms = (uint32_t)(s_consumed_bytes * 1000 / OUT_BYTES_PER_SEC);
        portEXIT_CRITICAL(&s_progress_mux);
    }
    return len;
}

/* ------------------------------------------------------------------ */
/* 解码任务                                                            */
/* ------------------------------------------------------------------ */
/*
 * 定点线性插值重采样（16.16），比浮点版快 5~8 倍；
 * 输入帧数上限 MAX_PCM_FRAMES，输出最多 2*MAX_PCM_FRAMES（低采样率升频时）。
 */
static int do_resample(const int16_t *in, int frames_in, int16_t *out)
{
    int in_rate = s_info.samplerate > 0 ? s_info.samplerate : PCM_RATE;
    if (in_rate == PCM_RATE) {
        memcpy(out, in, (size_t)frames_in * PCM_CH * sizeof(int16_t));
        return frames_in;
    }

    uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / (uint64_t)PCM_RATE);
    if (step == 0) {
        step = 1;
    }
    int frames_out = (int)(((uint64_t)frames_in * (uint64_t)PCM_RATE) / (uint64_t)in_rate);
    if (frames_out > MAX_PCM_FRAMES * 2) {
        frames_out = MAX_PCM_FRAMES * 2;
    }
    if (frames_out <= 0) {
        return 0;
    }

    uint32_t pos = 0;
    int last = frames_in - 1;
    for (int i = 0; i < frames_out; i++) {
        uint32_t idx = pos >> 16;
        if ((int)idx >= frames_in) {
            break;
        }
        int i0 = (int)idx;
        int i1 = (i0 < last) ? i0 + 1 : last;
        int32_t frac = (int32_t)(pos & 0xFFFF);
        for (int c = 0; c < PCM_CH; c++) {
            int32_t a = in[i0 * PCM_CH + c];
            int32_t b = in[i1 * PCM_CH + c];
            out[i * PCM_CH + c] = (int16_t)(a + (((b - a) * frac) >> 16));
        }
        pos += step;
    }
    return frames_out;
}

static void player_task(void *arg)
{
    (void)arg;
    uint32_t last_evt = 0;
    uint32_t last_diag = 0;
    uint32_t max_decode_us = 0;

    for (;;) {
        if ((s_state != PLAYER_PLAYING && s_state != PLAYER_LOADING) || s_dec == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_diag >= 5000) {
            last_diag = now;
            uint32_t missing = __atomic_exchange_n(&s_underrun_bytes, 0,
                                                    __ATOMIC_RELAXED);
            if (missing || max_decode_us >= 100000) {
                ESP_LOGW(TAG, "audio health: underrun=%lu bytes, max decode=%lu ms, buffer=%u%%",
                         (unsigned long)missing, (unsigned long)(max_decode_us / 1000),
                         player_buffer_level());
            }
            max_decode_us = 0;
        }
        if (now - last_evt > 400) {
            last_evt = now;
            app_evt_t evt = {.id = APP_EVT_TRACK_POS};
            evt.pos.ms = s_pos_ms;
            evt.pos.dur_ms = s_dur_ms;
            app_evt_post(&evt, 0);
        }

        xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
        if ((s_state != PLAYER_PLAYING && s_state != PLAYER_LOADING) || s_dec == NULL) {
            xSemaphoreGive(s_decoder_lock);
            continue;
        }

        if (s_seek_pending) {
            s_seek_pending = false;
            if (audio_dec_seek_ms(s_dec, s_seek_ms)) {
                s_eof_pending = false;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_head = s_tail = 0;
                xSemaphoreGive(s_lock);
                portENTER_CRITICAL(&s_progress_mux);
                s_consumed_bytes = (uint64_t)s_seek_ms * OUT_BYTES_PER_SEC / 1000;
                s_pos_ms = s_seek_ms;
                portEXIT_CRITICAL(&s_progress_mux);
            }
        }

        if (s_eof_pending) {
            /* 解码已结束，但 A2DP 仍可能有数秒 PCM 尚未取走。 */
            if (ring_used() == 0 && !music_lib_scanning()) {
                s_eof_pending = false;
                int total = music_lib_count();
                if (s_auto_next && total > 0) {
                    open_index_locked(next_library_index(current_library_index(), total, true), true);
                } else {
                    close_decoder();
                    set_state(PLAYER_STOPPED);
                }
            }
            xSemaphoreGive(s_decoder_lock);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 留出完整重采样块，避免低采样率曲目被 ring_write 截断。 */
        if (ring_free() < DECODE_RESERVE_BYTES) {
            xSemaphoreGive(s_decoder_lock);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int64_t decode_start_us = esp_timer_get_time();
        int n = audio_dec_read(s_dec, s_pcm, MAX_PCM_FRAMES);
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start_us);
        if (decode_us > max_decode_us) max_decode_us = decode_us;
        if (n <= 0) {
            if (!s_decoded_any_pcm) {
                if (s_initial_empty_reads++ < INITIAL_DECODE_RETRIES) {
                    ESP_LOGW(TAG, "initial audio read returned %d, retry %u/%u: %s",
                             n, (unsigned)s_initial_empty_reads,
                             (unsigned)INITIAL_DECODE_RETRIES, s_current.path);
                    xSemaphoreGive(s_decoder_lock);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "no audio frames: %s, format=%s, size=%lu bytes, position=%lu/%lu ms",
                         s_current.path, audio_dec_format_name(s_info.fmt),
                         (unsigned long)s_current.size,
                         (unsigned long)audio_dec_tell_ms(s_dec),
                         (unsigned long)s_dur_ms);
                close_decoder();
                set_state(PLAYER_ERROR);
                xSemaphoreGive(s_decoder_lock);
                continue;
            }
            if (n < 0) {
                ESP_LOGW(TAG, "decode error, skip track");
            }
            s_eof_pending = true;
            if (s_state == PLAYER_LOADING) set_state(PLAYER_PLAYING);
            xSemaphoreGive(s_decoder_lock);
            continue;
        }

        int frames = do_resample(s_pcm, n, s_resamp);
        if (frames > 0) s_decoded_any_pcm = true;
        analyze_pcm(s_resamp, frames);
        ring_write(s_resamp, (size_t)frames * PCM_CH * sizeof(int16_t));
        if (s_state == PLAYER_LOADING && ring_used() >= PREFILL_BYTES)
            set_state(PLAYER_PLAYING);
        xSemaphoreGive(s_decoder_lock);
    }
}

esp_err_t player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_decoder_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_decoder_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "PSRAM ring allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_pcm = heap_caps_malloc(MAX_PCM_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_resamp = heap_caps_malloc(MAX_PCM_FRAMES * 2 * sizeof(int16_t) * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || !s_pcm || !s_resamp) {
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t task_ok = xTaskCreate(player_task, "player", 16384, NULL, 7, &s_task);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "player ready (ring %u KB)", (unsigned)(RING_BYTES / 1024));
    return ESP_OK;
}

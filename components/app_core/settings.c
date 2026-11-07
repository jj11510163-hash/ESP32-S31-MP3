#include "app_core.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NVS_NS = "s31p";
static const char *NVS_KEY = "cfg";

static app_settings_t s_cfg;
static bool s_ready;
static bool s_dirty;

static void settings_defaults(app_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->volume = 26; /* 20% on the AVRCP 0..127 scale */
    cfg->brightness = 3;
    cfg->eq = 0;
    cfg->auto_reconnect = 1;
    cfg->screen_timeout_s = 0;
    cfg->theme = 0;
    cfg->last_index = 0;
    cfg->shuffle = 0;
    cfg->repeat = 1;
    strcpy(cfg->last_peer_addr, "");
    strcpy(cfg->last_peer_name, "");
}

static void settings_sanitize(app_settings_t *cfg)
{
    if (cfg->volume > 127) {
        cfg->volume = 127;
    }
    if (cfg->brightness > 4) {
        cfg->brightness = 4;
    }
    if (cfg->screen_timeout_s != 0 && cfg->screen_timeout_s != 15 &&
        cfg->screen_timeout_s != 30 && cfg->screen_timeout_s != 60 &&
        cfg->screen_timeout_s != 120) {
        cfg->screen_timeout_s = 0;
    }
    if (cfg->eq > 3) {
        cfg->eq = 0;
    }
    if (cfg->bt_codec > 2) {
        cfg->bt_codec = 0;
    }
    if (cfg->theme > 1) {
        cfg->theme = 0;
    }
    cfg->shuffle = cfg->shuffle ? 1 : 0;
    /* Playback modes are mutually exclusive. */
    if (cfg->repeat != 2 || cfg->shuffle) {
        cfg->repeat = 1;
    }
    cfg->last_peer_addr[17] = '\0';
    cfg->last_peer_name[31] = '\0';
}

esp_err_t app_settings_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    settings_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        app_settings_t loaded;
        size_t len = sizeof(loaded);
        if (nvs_get_blob(h, NVS_KEY, &loaded, &len) == ESP_OK && len == sizeof(loaded)) {
            s_cfg = loaded;
            settings_sanitize(&s_cfg);
            ESP_LOGI(TAG, "loaded: vol=%d bright=%d peer=%s", s_cfg.volume, s_cfg.brightness,
                     s_cfg.last_peer_name[0] ? s_cfg.last_peer_name : "-");
        } else {
            ESP_LOGI(TAG, "no stored config, using defaults");
            s_dirty = true;
        }
        nvs_close(h);
    } else {
        s_dirty = true;
    }

    s_ready = true;
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_cfg;
}

esp_err_t app_settings_save(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_dirty = false;
    } else {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_settings_update(const app_settings_t *new_cfg)
{
    if (!s_ready || new_cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 保留运行时字段保护 */
    s_cfg = *new_cfg;
    settings_sanitize(&s_cfg);
    s_dirty = true;
    esp_err_t err = app_settings_save();
    app_evt_post_id(APP_EVT_SETTINGS);
    return err;
}

esp_err_t app_settings_set_volume(uint8_t vol)
{
    if (vol > 127) {
        vol = 127;
    }
    if (s_cfg.volume == vol) {
        return ESP_OK;
    }
    s_cfg.volume = vol;
    s_dirty = true;

    app_evt_t evt = {0};
    evt.id = APP_EVT_BT_VOLUME;
    evt.vol.value = vol;
    app_evt_post(&evt, 0);

    /* 音量是高频操作，延迟落盘由调用方或关机前统一保存 */
    return ESP_OK;
}

esp_err_t app_settings_volume_step(int8_t delta, uint8_t step)
{
    int32_t v = (int32_t)s_cfg.volume + (int32_t)delta * (int32_t)step;
    if (v < 0) {
        v = 0;
    }
    if (v > 127) {
        v = 127;
    }
    return app_settings_set_volume((uint8_t)v);
}

esp_err_t app_settings_set_brightness(uint8_t level)
{
    if (level > 4) {
        level = 4;
    }
    s_cfg.brightness = level;
    s_dirty = true;
    app_evt_post_id(APP_EVT_SETTINGS);
    return app_settings_save();
}

bool app_settings_dirty(void)
{
    return s_dirty;
}

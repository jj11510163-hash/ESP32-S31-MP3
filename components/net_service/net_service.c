/*
 * net_service.c —— Wi-Fi AP + HTTP 传歌
 *
 * 开启后手机连 "S31-Player-xxxx"（密码 s31player），浏览器打开 http://192.168.4.1/
 * 即可上传歌曲到 SD 卡的 /music 目录；上传完成自动刷新曲库。
 */
#include "net_service.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "app_core.h"
#include "bsp.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music.h"

static const char *TAG = "net";

static bool s_inited;
static bool s_ap_running;
static uint8_t s_clients;
static char s_ssid[32];
static httpd_handle_t s_http;
static volatile uint8_t s_upload_pct;
static int s_sd_active;
static SemaphoreHandle_t s_sd_mutex;
static uint32_t s_upload_seq;
static esp_timer_handle_t s_rescan_timer;
static bool s_rescan_pending;
static uint32_t s_rescan_epoch;
static bool s_upload_parts_cleaned;

#define WEB_NAME_MAX 240
#define WEB_QUERY_MAX 512
#define UPLOAD_BUF_SIZE 4096
#define UPLOAD_TIMEOUT_RETRIES 12
#define UPLOAD_SPACE_GUARD (32U * 1024U)
#define UPLOAD_RESCAN_DELAY_US (10LL * 1000 * 1000)

extern const char index_html_start[] asm("_binary_index_html_start");

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            s_clients++;
            ESP_LOGI(TAG, "client connected (%d)", s_clients);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (s_clients) {
                s_clients--;
            }
            ESP_LOGI(TAG, "client left (%d)", s_clients);
        }
    }
    app_evt_t evt = {.id = APP_EVT_NET_STATE};
    evt.net.ap_running = s_ap_running;
    evt.net.clients = s_clients;
    app_evt_post(&evt, 0);
}

/* ------------------------------------------------------------------ */
/* HTTP 端点                                                           */
/* ------------------------------------------------------------------ */
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t text_reply(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

static bool is_upload_part(const char *name)
{
    if (strcmp(name, ".upload.part") == 0) return true;
    static const char prefix[] = ".upload-";
    static const char suffix[] = ".part";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strlen(name) != prefix_len + 8 + sizeof(suffix) - 1 ||
        strncmp(name, prefix, prefix_len) != 0 ||
        strcmp(name + prefix_len + 8, suffix) != 0) return false;
    for (size_t i = prefix_len; i < prefix_len + 8; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static void cleanup_upload_parts(void)
{
    DIR *dir = opendir(MUSIC_SCAN_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_upload_part(entry->d_name)) continue;
        char path[MUSIC_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", MUSIC_SCAN_DIR, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (remove(path) == 0) ESP_LOGI(TAG, "removed stale upload %s", entry->d_name);
            else ESP_LOGW(TAG, "could not remove stale upload %s: %d", entry->d_name, errno);
        }
    }
    closedir(dir);
}

static esp_err_t sd_request(httpd_req_t *req, esp_err_t (*handler)(httpd_req_t *))
{
    if (music_update_mode())
        return text_reply(req, "503 Service Unavailable", "固件更新中");

    __atomic_add_fetch(&s_sd_active, 1, __ATOMIC_ACQ_REL);
    if (!s_sd_mutex || xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        __atomic_sub_fetch(&s_sd_active, 1, __ATOMIC_ACQ_REL);
        return text_reply(req, "503 Service Unavailable", "存储卡正忙，请稍后重试");
    }
    if (music_update_mode()) {
        xSemaphoreGive(s_sd_mutex);
        __atomic_sub_fetch(&s_sd_active, 1, __ATOMIC_ACQ_REL);
        return text_reply(req, "503 Service Unavailable", "固件更新中");
    }
    if (!bsp_sd_is_mounted()) {
        s_upload_parts_cleaned = false;
    } else if (!s_upload_parts_cleaned && !music_lib_scanning()) {
        // The SD request mutex keeps this sweep away from an in-progress web upload.
        cleanup_upload_parts();
        s_upload_parts_cleaned = true;
    }
    esp_err_t err = handler(req);
    xSemaphoreGive(s_sd_mutex);
    __atomic_sub_fetch(&s_sd_active, 1, __ATOMIC_ACQ_REL);
    return err;
}

static void deferred_rescan(void *arg)
{
    (void)arg;
    if (!__atomic_load_n(&s_rescan_pending, __ATOMIC_ACQUIRE)) return;
    uint32_t epoch = __atomic_load_n(&s_rescan_epoch, __ATOMIC_ACQUIRE);
    if (music_update_mode() || net_service_sd_busy()) {
        if (s_rescan_timer && __atomic_load_n(&s_rescan_pending, __ATOMIC_ACQUIRE))
            esp_timer_start_once(s_rescan_timer, 1000000);
        return;
    }
    if (__atomic_exchange_n(&s_rescan_pending, false, __ATOMIC_ACQ_REL)) {
        esp_err_t err = net_service_notify_lib_changed();
        if (err != ESP_OK && epoch == __atomic_load_n(&s_rescan_epoch, __ATOMIC_ACQUIRE)) {
            ESP_LOGW(TAG, "deferred library rescan failed: %s; retrying", esp_err_to_name(err));
            __atomic_store_n(&s_rescan_pending, true, __ATOMIC_RELEASE);
            if (s_rescan_timer) esp_timer_start_once(s_rescan_timer, 1000000);
        }
    }
}

static void schedule_rescan(void)
{
    __atomic_add_fetch(&s_rescan_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_rescan_pending, true, __ATOMIC_RELEASE);
    if (!s_rescan_timer) {
        __atomic_store_n(&s_rescan_pending, false, __ATOMIC_RELEASE);
        net_service_notify_lib_changed();
        return;
    }
    esp_timer_stop(s_rescan_timer);
    esp_timer_start_once(s_rescan_timer, UPLOAD_RESCAN_DELAY_US);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        unsigned n;
        if (*p < 0x80) {
            p++;
            continue;
        }
        if ((*p & 0xe0) == 0xc0) { cp = *p & 0x1f; n = 1; }
        else if ((*p & 0xf0) == 0xe0) { cp = *p & 0x0f; n = 2; }
        else if ((*p & 0xf8) == 0xf0) { cp = *p & 0x07; n = 3; }
        else return false;
        p++;
        for (unsigned i = 0; i < n; i++) {
            if (*p == 0 || (*p & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (*p++ & 0x3f);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
            (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

/* The browser percent-encodes UTF-8 filenames; ESP-IDF's query helper does not decode them. */
static bool request_name(httpd_req_t *req, char name[WEB_NAME_MAX], bool allow_folders)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= WEB_QUERY_MAX) return false;
    char query[WEB_QUERY_MAX];
    char encoded[WEB_QUERY_MAX];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded, sizeof(encoded)) != ESP_OK) return false;

    size_t out = 0;
    for (size_t i = 0; encoded[i]; i++) {
        unsigned char ch = (unsigned char)encoded[i];
        if (ch == '%') {
            int hi = hex_digit(encoded[++i]);
            if (hi < 0 || !encoded[i + 1]) return false;
            int lo = hex_digit(encoded[++i]);
            if (lo < 0) return false;
            ch = (unsigned char)((hi << 4) | lo);
        } else if (ch == '+') {
            ch = ' ';
        }
        if (out + 1 >= WEB_NAME_MAX || ch < 0x20 || ch == 0x7f ||
            (ch == '/' && !allow_folders) || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') return false;
        name[out++] = (char)ch;
    }
    name[out] = '\0';
    if (out == 0 || !valid_utf8(name) || music_fmt_from_path(name) == MUSIC_FMT_UNKNOWN) return false;
    size_t segment = 0;
    for (size_t i = 0; i <= out; i++) {
        if (name[i] == '/' || name[i] == '\0') {
            if (i == segment || name[segment] == '.' || name[i - 1] == '.' || name[i - 1] == ' ')
                return false;
            segment = i + 1;
        }
    }
    return true;
}

static bool music_path(char path[MUSIC_PATH_MAX], const char *name)
{
    int n = snprintf(path, MUSIC_PATH_MAX, "%s/%s", MUSIC_SCAN_DIR, name);
    return n > 0 && n < MUSIC_PATH_MAX;
}

static bool card_path(char path[MUSIC_PATH_MAX], const char *name)
{
    int n = snprintf(path, MUSIC_PATH_MAX, "%s/%s", BSP_SD_MOUNT_POINT, name);
    return n > 0 && n < MUSIC_PATH_MAX;
}

static bool json_escape(char *dst, size_t cap, const char *src)
{
    static const char digits[] = "0123456789abcdef";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p < 0x20) {
            if (o + 6 >= cap) return false;
            dst[o++] = '\\'; dst[o++] = 'u'; dst[o++] = '0'; dst[o++] = '0';
            dst[o++] = digits[*p >> 4]; dst[o++] = digits[*p & 15];
        } else {
            if (o + ((*p == '"' || *p == '\\') ? 2 : 1) >= cap) return false;
            if (*p == '"' || *p == '\\') dst[o++] = '\\';
            dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
    return true;
}

typedef struct {
    httpd_req_t *req;
    bool first;
    char escaped[WEB_NAME_MAX * 6 + 1];
    char item[WEB_NAME_MAX * 6 + 80];
} files_context_t;

static esp_err_t files_walk(files_context_t *ctx, const char *folder, const char *prefix, int depth)
{
    DIR *dir = opendir(folder);
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    esp_err_t result = ESP_OK;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char relative[WEB_NAME_MAX];
        int n = snprintf(relative, sizeof(relative), "%s%s", prefix, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(relative) || !valid_utf8(relative)) continue;
        char path[MUSIC_PATH_MAX];
        if (!card_path(path, relative)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) {
                char child_prefix[WEB_NAME_MAX];
                n = snprintf(child_prefix, sizeof(child_prefix), "%s/", relative);
                if (n > 0 && n < (int)sizeof(child_prefix))
                    result = files_walk(ctx, path, child_prefix, depth + 1);
            }
        } else if (S_ISREG(st.st_mode) && music_fmt_from_path(entry->d_name) != MUSIC_FMT_UNKNOWN) {
            if (!json_escape(ctx->escaped, sizeof(ctx->escaped), relative)) continue;
            n = snprintf(ctx->item, sizeof(ctx->item), "%s{\"name\":\"%s\",\"size\":%llu}",
                         ctx->first ? "" : ",", ctx->escaped, (unsigned long long)st.st_size);
            if (n < 0 || n >= (int)sizeof(ctx->item) ||
                httpd_resp_send_chunk(ctx->req, ctx->item, n) != ESP_OK)
                result = ESP_FAIL;
            else
                ctx->first = false;
        }
        if (result != ESP_OK) break;
    }
    closedir(dir);
    return result;
}

static esp_err_t files_get_impl(httpd_req_t *req)
{
    bool mounted = bsp_sd_is_mounted();
    uint64_t total = 0, free_bytes = 0;
    if (mounted) bsp_sd_info(&total, &free_bytes);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[128];
    snprintf(head, sizeof(head), "{\"mounted\":%s,\"total\":%llu,\"free\":%llu,\"files\":[",
             mounted ? "true" : "false", (unsigned long long)total, (unsigned long long)free_bytes);
    if (httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;

    files_context_t ctx = {.req = req, .first = true};
    if (mounted && files_walk(&ctx, BSP_SD_MOUNT_POINT, "", 0) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool upload_wants_rescan(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX];
    char scan[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "scan", scan, sizeof(scan)) != ESP_OK) {
        return true;
    }
    return strcmp(scan, "0") != 0;
}

static esp_err_t upload_post_impl(httpd_req_t *req)
{
    if (!bsp_sd_is_mounted()) return text_reply(req, "503 Service Unavailable", "未检测到存储卡");
    char name[WEB_NAME_MAX];
    char path[MUSIC_PATH_MAX];
    if (!request_name(req, name, false) || !music_path(path, name))
        return text_reply(req, "400 Bad Request", "文件名无效或格式不支持");
    if (req->content_len == 0 || req->content_len > UINT32_MAX)
        return text_reply(req, "413 Content Too Large", "文件大小不支持");
    struct stat st;
    if (stat(path, &st) == 0) return text_reply(req, "409 Conflict", "同名歌曲已存在");

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t space_err = bsp_sd_info(&total_bytes, &free_bytes);
    if (space_err == ESP_OK && free_bytes < (uint64_t)req->content_len + UPLOAD_SPACE_GUARD)
        return text_reply(req, "507 Insufficient Storage", "存储卡空间不足");

    if (mkdir(MUSIC_SCAN_DIR, 0777) != 0 && errno != EEXIST)
        return text_reply(req, "500 Internal Server Error", "无法创建音乐目录");

    uint32_t sequence = __atomic_add_fetch(&s_upload_seq, 1, __ATOMIC_RELAXED);
    char temp_path[MUSIC_PATH_MAX];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s/.upload-%08lx.part",
                            MUSIC_SCAN_DIR, (unsigned long)sequence);
    if (temp_len <= 0 || temp_len >= (int)sizeof(temp_path))
        return text_reply(req, "500 Internal Server Error", "临时文件名过长");
    FILE *fp = fopen(temp_path, "wb");
    if (!fp) return text_reply(req, "500 Internal Server Error", "无法写入存储卡");

    size_t remaining = req->content_len;
    char *buf = (char *)malloc(UPLOAD_BUF_SIZE);
    if (!buf) {
        fclose(fp);
        remove(temp_path);
        return text_reply(req, "503 Service Unavailable", "内存不足，请稍后重试");
    }
    int timeouts = 0;
    bool ok = true;
    s_upload_pct = 0;
    ESP_LOGI(TAG, "upload %s (%u bytes)", name, (unsigned)remaining);
    while (remaining > 0) {
        size_t to_read = remaining < UPLOAD_BUF_SIZE ? remaining : UPLOAD_BUF_SIZE;
        int r = httpd_req_recv(req, buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= UPLOAD_TIMEOUT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (r <= 0) {
            ok = false;
            break;
        }
        timeouts = 0;
        if (fwrite(buf, 1, (size_t)r, fp) != (size_t)r) {
            ok = false;
            break;
        }
        remaining -= r;
        uint8_t pct = (uint8_t)(((uint64_t)(req->content_len - remaining) * 100) / req->content_len);
        if (pct != s_upload_pct) {
            s_upload_pct = pct;
            app_evt_t evt = {.id = APP_EVT_UPLOAD_PROGRESS};
            evt.upload.percent = pct;
            app_evt_post(&evt, 0);
        }
    }
    if (fclose(fp) != 0) ok = false;
    free(buf);
    if (!ok) {
        remove(temp_path);
        return text_reply(req, "500 Internal Server Error", "传输或写入失败");
    }
    if (stat(path, &st) == 0) {
        remove(temp_path);
        return text_reply(req, "409 Conflict", "同名歌曲已存在");
    }
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return text_reply(req, "500 Internal Server Error", "保存歌曲失败");
    }
    ESP_LOGI(TAG, "saved %s", path);
    if (upload_wants_rescan(req))
        net_service_notify_lib_changed();
    else
        schedule_rescan();
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t rescan_post_impl(httpd_req_t *req)
{
    if (!bsp_sd_is_mounted()) return text_reply(req, "503 Service Unavailable", "未检测到存储卡");
    __atomic_add_fetch(&s_rescan_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_rescan_pending, false, __ATOMIC_RELEASE);
    if (s_rescan_timer) esp_timer_stop(s_rescan_timer);
    esp_err_t err = net_service_notify_lib_changed();
    if (err != ESP_OK) {
        schedule_rescan();
        return text_reply(req, "503 Service Unavailable", "曲库刷新失败");
    }
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t delete_post_impl(httpd_req_t *req)
{
    if (!bsp_sd_is_mounted()) return text_reply(req, "503 Service Unavailable", "未检测到存储卡");
    char name[WEB_NAME_MAX];
    char path[MUSIC_PATH_MAX];
    if (!request_name(req, name, true) || !card_path(path, name))
        return text_reply(req, "400 Bad Request", "文件名无效");
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return text_reply(req, "404 Not Found", "歌曲不存在");

    const music_item_t *current = player_get_current();
    if (current && strcmp(current->path, path) == 0) {
        player_stop();
    }
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "delete %s failed: %d", path, errno);
        return text_reply(req, "500 Internal Server Error", "删除失败");
    }
    ESP_LOGI(TAG, "deleted %s", path);
    net_service_notify_lib_changed();
    return text_reply(req, "200 OK", "OK");
}

static esp_err_t files_get(httpd_req_t *req)
{
    return sd_request(req, files_get_impl);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    return sd_request(req, upload_post_impl);
}

static esp_err_t delete_post(httpd_req_t *req)
{
    return sd_request(req, delete_post_impl);
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    return sd_request(req, rescan_post_impl);
}

/* 遥控按键：/api/key?k=up|down|left|right|ok|back */
static esp_err_t key_get(httpd_req_t *req)
{
    char q[64] = {0};
    char k[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "k", k, sizeof(k)) == ESP_OK) {
        app_key_t key = APP_KEY_MAX;
        if (strcmp(k, "up") == 0) {
            key = APP_KEY_UP;
        } else if (strcmp(k, "down") == 0) {
            key = APP_KEY_DOWN;
        } else if (strcmp(k, "left") == 0) {
            key = APP_KEY_PAGE_PREV;
        } else if (strcmp(k, "right") == 0) {
            key = APP_KEY_PAGE_NEXT;
        } else if (strcmp(k, "ok") == 0) {
            key = APP_KEY_ENTER;
        } else if (strcmp(k, "back") == 0) {
            key = APP_KEY_BACK;
        }
        if (key != APP_KEY_MAX) {
            app_input_post(key, 1);
            ESP_LOGI(TAG, "remote key: %s", k);
        }
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t start_http(void)
{
    if (s_http) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 9;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 12288;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        s_http = NULL;
        return ESP_FAIL;
    }
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_get, .user_ctx = NULL};
    httpd_uri_t files = {.uri = "/api/files", .method = HTTP_GET, .handler = files_get, .user_ctx = NULL};
    httpd_uri_t up = {.uri = "/api/upload", .method = HTTP_POST, .handler = upload_post, .user_ctx = NULL};
    httpd_uri_t del = {.uri = "/api/delete", .method = HTTP_POST, .handler = delete_post, .user_ctx = NULL};
    httpd_uri_t rescan = {.uri = "/api/rescan", .method = HTTP_POST, .handler = rescan_post, .user_ctx = NULL};
    httpd_uri_t key = {.uri = "/api/key", .method = HTTP_GET, .handler = key_get, .user_ctx = NULL};
    httpd_register_uri_handler(s_http, &index);
    httpd_register_uri_handler(s_http, &files);
    httpd_register_uri_handler(s_http, &up);
    httpd_register_uri_handler(s_http, &del);
    httpd_register_uri_handler(s_http, &rescan);
    httpd_register_uri_handler(s_http, &key);
    ESP_LOGI(TAG, "http server on :80");
    return ESP_OK;
}

esp_err_t net_service_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));

    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) return ESP_ERR_NO_MEM;
    esp_timer_create_args_t timer_args = {
        .callback = deferred_rescan,
        .name = "web_rescan",
    };
    err = esp_timer_create(&timer_args, &s_rescan_timer);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_sd_mutex);
        s_sd_mutex = NULL;
        return err;
    }
    if (bsp_sd_is_mounted() && !music_lib_scanning() &&
        xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        cleanup_upload_parts();
        s_upload_parts_cleaned = true;
        xSemaphoreGive(s_sd_mutex);
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "%s-%02X%02X", NET_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t wcfg = {0};
    snprintf((char *)wcfg.ap.ssid, sizeof(wcfg.ap.ssid), "%s", s_ssid);
    wcfg.ap.ssid_len = (uint8_t)strlen(s_ssid);
    snprintf((char *)wcfg.ap.password, sizeof(wcfg.ap.password), "%s", NET_AP_PASSWORD);
    wcfg.ap.max_connection = NET_AP_MAX_CLIENTS;
    wcfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.ap.channel = 6;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));

    s_inited = true;
    ESP_LOGI(TAG, "AP configured: %s", s_ssid);
    return ESP_OK;
}

esp_err_t net_service_ap_start(void)
{
    if (music_update_mode()) return ESP_ERR_INVALID_STATE;
    if (!s_inited) {
        esp_err_t e = net_service_init();
        if (e != ESP_OK) {
            return e;
        }
    }
    if (s_ap_running) {
        return ESP_OK;
    }
    s_clients = 0;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ap_running = true;
    err = start_http();
    if (err != ESP_OK) {
        esp_wifi_stop();
        s_ap_running = false;
        return err;
    }
    app_settings_t cfg = *app_settings_get();
    (void)cfg;
    app_evt_t evt = {.id = APP_EVT_NET_STATE};
    evt.net.ap_running = true;
    evt.net.clients = s_clients;
    app_evt_post(&evt, 0);
    ESP_LOGI(TAG, "AP started: %s  http://%s/", s_ssid, net_service_ap_ip());
    return ESP_OK;
}

esp_err_t net_service_ap_stop(void)
{
    if (!s_ap_running) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "wifi stop failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ap_running = false;
    s_clients = 0;
    app_evt_t evt = {.id = APP_EVT_NET_STATE};
    evt.net.ap_running = false;
    evt.net.clients = 0;
    app_evt_post(&evt, 0);
    return ESP_OK;
}

bool net_service_ap_running(void)
{
    return s_ap_running;
}

bool net_service_sd_busy(void)
{
    return __atomic_load_n(&s_sd_active, __ATOMIC_ACQUIRE) != 0;
}

uint8_t net_service_clients(void)
{
    return s_ap_running ? s_clients : 0;
}

const char *net_service_ap_ssid(void)
{
    return s_ssid[0] ? s_ssid : "S31-Player";
}

const char *net_service_ap_ip(void)
{
    return "192.168.4.1";
}

esp_err_t net_service_notify_lib_changed(void)
{
    return music_lib_rescan_async();
}

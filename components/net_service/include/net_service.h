/* Phone connects to the local Wi-Fi AP and opens http://192.168.4.1/.
 * The web UI lists audio files on the card, uploads to /sdcard/music, and deletes
 * selected files through /api/files, /api/upload and /api/delete. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_AP_SSID_PREFIX "S31-Player"
#define NET_AP_PASSWORD "s31player"
#define NET_AP_MAX_CLIENTS 4

esp_err_t net_service_init(void);
esp_err_t net_service_ap_start(void);
esp_err_t net_service_ap_stop(void);
bool net_service_ap_running(void);
bool net_service_sd_busy(void);
uint8_t net_service_clients(void);
const char *net_service_ap_ssid(void);
const char *net_service_ap_ip(void);

/* 上传落盘后调用，触发曲库刷新 */
esp_err_t net_service_notify_lib_changed(void);

#ifdef __cplusplus
}
#endif

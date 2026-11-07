#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_UPDATE_PATH "/sdcard/update/firmware.bin"

typedef enum {
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_PREPARING,
    FW_UPDATE_RUNNING,
    FW_UPDATE_SUCCESS,
    FW_UPDATE_FAILED,
} fw_update_state_t;

typedef struct {
    fw_update_state_t state;
    uint8_t percent;
    uint32_t image_size;
    bool available;
    char current_version[32];
    char candidate_version[32];
    char message[64];
} fw_update_info_t;

/* Read only the SD image header; call when entering the update page. */
esp_err_t fw_update_probe(fw_update_info_t *out);

/* Start a one-shot background update. Status remains visible until next probe. */
esp_err_t fw_update_start(void);
void fw_update_get_status(fw_update_info_t *out);
bool fw_update_busy(void);

#ifdef __cplusplus
}
#endif

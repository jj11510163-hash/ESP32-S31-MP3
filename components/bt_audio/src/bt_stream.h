#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_a2dp_api.h"
#include "esp_err.h"

esp_err_t bt_stream_start(esp_a2d_conn_hdl_t conn, uint16_t mtu, const esp_a2d_mcc_t *mcc,
                          uint32_t generation);
void bt_stream_stop(void);
bool bt_stream_take_failure(uint32_t *generation, esp_a2d_conn_hdl_t *conn, uint8_t *codec);


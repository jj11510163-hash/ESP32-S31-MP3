#include "app_core.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "evt";

#define APP_EVT_QUEUE_LEN 32

static QueueHandle_t s_queue;

esp_err_t app_events_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(APP_EVT_QUEUE_LEN, sizeof(app_evt_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "event bus ready (%d slots, %u bytes each)", APP_EVT_QUEUE_LEN, (unsigned)sizeof(app_evt_t));
    return ESP_OK;
}

esp_err_t app_evt_post(const app_evt_t *evt, uint32_t wait_ms)
{
    if (s_queue == NULL || evt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_queue, evt, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_evt_post_id(app_evt_id_t id)
{
    app_evt_t evt = {0};
    evt.id = id;
    return app_evt_post(&evt, 0);
}

esp_err_t app_evt_post_isr(const app_evt_t *evt, int32_t *hp_woken)
{
    if (s_queue == NULL || evt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t woken = pdFALSE;
    BaseType_t ok = xQueueSendFromISR(s_queue, evt, &woken);
    if (hp_woken) {
        *hp_woken = (int32_t)woken;
    }
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool app_evt_wait(app_evt_t *out, uint32_t wait_ms)
{
    if (s_queue == NULL || out == NULL) {
        return false;
    }
    return xQueueReceive(s_queue, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

uint32_t app_evt_pending(void)
{
    return (s_queue != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
}

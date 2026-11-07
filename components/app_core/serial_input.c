/*
 * serial_input.c —— 串口遥控
 *
 * 在 idf_monitor 里直接敲字母就能操作设备（不需要外接按键）：
 *   W 上    A 左    S 下    D 右    R 确认    F 返回/删除    P 播放暂停
 *   1/2/3 模拟 KEY0 单/双/三击    L 模拟长按
 *   M 重新挂载 SD 卡（诊断用）   I 打印系统信息
 *
 * 走 IDF 的 UART VFS 驱动，和 printf 共用同一个串口。
 */
#include "app_core.h"


#include "bsp.h"
#include "music.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "keys";

static void print_info(void)
{
    uint64_t total = 0, free_b = 0;
    bool mounted = bsp_sd_is_mounted();
    if (mounted) {
        bsp_sd_info(&total, &free_b);
    }
    ESP_LOGI(TAG, "===== 系统 =====");
    ESP_LOGI(TAG, "SD: %s  total=%uM free=%uM", mounted ? "已挂载" : "未挂载",
             (unsigned)(total / (1024 * 1024)), (unsigned)(free_b / (1024 * 1024)));
    ESP_LOGI(TAG, "PSRAM free: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "heap  free: %u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    ESP_LOGI(TAG, "曲库: %d 首, 扫描中=%d", music_lib_count(), (int)music_lib_scanning());
    ESP_LOGI(TAG, "================");
}

static void serial_task(void *arg)
{
    (void)arg;
    uint8_t ch = 0;

    for (;;) {
        /* 直接走 UART 驱动的环形缓冲：不经过 newlib stdio，
           避免与日志输出在同一任务里重入导致栈保护异常 */
        int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(50));
        if (n != 1) {
            continue;
        }
        switch (ch) {
        case 'w':
        case 'W':
            app_input_post(APP_KEY_UP, 1);
            break;
        case 's':
        case 'S':
            app_input_post(APP_KEY_DOWN, 1);
            break;
        case 'a':
        case 'A':
            app_input_post(APP_KEY_ROT_CCW, 1);
            break;
        case 'd':
        case 'D':
            app_input_post(APP_KEY_ROT_CW, 1);
            break;
        case 'r':
        case 'R':
            app_input_post(APP_KEY_ENTER, 1);
            break;
        case 'f':
        case 'F':
            app_input_post(APP_KEY_BACK, 1);
            break;
        case 'p':
        case 'P':
            app_input_post(APP_KEY_PLAY, 1);
            break;
        case '1':
            app_input_post(APP_KEY_KEY0_SINGLE, 1);
            break;
        case '2':
            app_input_post(APP_KEY_KEY0_DOUBLE, 1);
            break;
        case '3':
            app_input_post(APP_KEY_KEY0_TRIPLE, 1);
            break;
        case 'l':
        case 'L':
            app_input_post(APP_KEY_KEY0_LONG, 1);
            break;
        case 'm':
        case 'M':
            ESP_LOGW(TAG, "手动重新挂载 SD ...");
            if (bsp_sd_mount() == ESP_OK) {
                ESP_LOGI(TAG, "SD 挂载成功");
                music_lib_scan();
            } else {
                ESP_LOGE(TAG, "SD 仍然挂载失败，见上面 bsp_sd 的错误码");
            }
            break;
        case 'i':
        case 'I':
            print_info();
            break;
        default:
            break;
        }
    }
}

esp_err_t app_serial_input_start(void)
{
    /* Install on the main task stack and replace the console driver so
     * uart_read_bytes() has an RX buffer. */
    if (uart_is_driver_installed(UART_NUM_0)) {
        ESP_LOGW(TAG, "UART0 driver 已存在，重建以启用 RX 缓冲");
        uart_driver_delete(UART_NUM_0);
    }
    esp_err_t uerr = uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    if (uerr != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install 失败: %s", esp_err_to_name(uerr));
        return uerr;
    }
    uart_vfs_dev_use_driver(UART_NUM_0);

    ESP_LOGI(TAG, "串口遥控就绪：W/A/S/D 移动 R 确认 F 返回 P 播放 1/2/3/L 模拟 KEY0 M 重挂 SD I 信息");

    BaseType_t ok = xTaskCreate(serial_task, "keys", 8192, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

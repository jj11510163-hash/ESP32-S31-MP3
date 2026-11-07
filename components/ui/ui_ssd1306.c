#include "ui_ssd1306.h"

#include <string.h>

#include "bsp.h"
#include "esp_log.h"
#include "ui_gfx.h"

static const char *TAG = "ssd1306";

#define CTRL_CMD 0x00
#define CTRL_DATA 0x40

static i2c_master_dev_handle_t s_dev;
static uint8_t s_col_offset;
static uint8_t s_addr;
static uint8_t s_col_buf[1 + UI_W];

static esp_err_t wr(const uint8_t *data, size_t len)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_dev, data, len, 100);
}

static esp_err_t cmd_list(const uint8_t *cmds, size_t n)
{
    uint8_t buf[64];
    if (n + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return wr(buf, n + 1);
}

#define CMD1(c)                       \
    do {                              \
        const uint8_t _c = (c);       \
        cmd_list(&_c, 1);             \
    } while (0)

esp_err_t ssd1306_init(void)
{
    if (s_dev) {
        return ESP_OK;
    }
    if (bsp_i2c_bus() == NULL) {
        if (bsp_i2c_init() != ESP_OK) {
            return ESP_FAIL;
        }
    }

    static const uint8_t candidates[] = {SSD1306_I2C_ADDR_PRIMARY, SSD1306_I2C_ADDR_SECONDARY};
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (i2c_master_probe(bsp_i2c_bus(), candidates[i], 60) == ESP_OK) {
            if (bsp_i2c_add_device(candidates[i], BSP_I2C_FREQ_HZ, &s_dev) == ESP_OK) {
                s_addr = candidates[i];
                ESP_LOGI(TAG, "found panel at 0x%02X", s_addr);
                break;
            }
        }
    }
    if (!s_dev) {
        ESP_LOGW(TAG, "no SSD1306 on I2C bus (SDA=IO%d SCL=IO%d)", BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
        return ESP_ERR_NOT_FOUND;
    }

    static const uint8_t init_seq[] = {
        0xAE,       /* display off            */
        0xD5, 0x80, /* clock divide           */
        0xA8, 0x3F, /* multiplex 1/64         */
        0xD3, 0x00, /* display offset         */
        0x40,       /* start line 0           */
        0x8D, 0x14, /* charge pump on         */
        0x20, 0x00, /* memory addressing: page*/
        0xA1,       /* segment remap          */
        0xC8,       /* COM scan direction     */
        0xDA, 0x12, /* COM pins               */
        0x81, 0xCF, /* contrast               */
        0xD9, 0xF1, /* pre-charge             */
        0xDB, 0x40, /* VCOMH                  */
        0xA4,       /* output follows RAM     */
        0xA6,       /* normal (not inverted)  */
        0x2E,       /* scroll off             */
        0xAF,       /* display on             */
    };
    /*
     * SH1106（1.3 寸屏常见）GDDRAM 是 132 列，可见区从第 2 列开始。
     * SSD1306 支持读回，SH1106 不支持——用它来区分：读失败就按 SH1106 处理，
     * 显示内容整体右移 2 列，最右边的花屏就没了。
     */
    /* 1.3 寸模块绝大多数是 SH1106（132 列 GDDRAM，可见区从第 2 列起）。
       写 128 列时如果不偏移，右边会漏出 4 列未初始化数据 = 花屏。
       读回探测不可靠（SH1106 也会 ACK），这里直接按 2 处理；
       若是纯 SSD1306，可调 ssd1306_set_col_offset(0)。 */
    s_col_offset = 2;

    esp_err_t err = cmd_list(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool ssd1306_present(void)
{
    return s_dev != NULL;
}

uint8_t ssd1306_addr(void)
{
    return s_addr;
}

esp_err_t ssd1306_set_contrast(uint8_t contrast)
{
    const uint8_t c[2] = {0x81, contrast};
    return cmd_list(c, 2);
}

esp_err_t ssd1306_display_on(bool on)
{
    CMD1(on ? 0xAF : 0xAE);
    return ESP_OK;
}

esp_err_t ssd1306_invert(bool on)
{
    CMD1(on ? 0xA7 : 0xA6);
    return ESP_OK;
}

void ssd1306_set_col_offset(uint8_t off)
{
    s_col_offset = off;
}

uint8_t ssd1306_col_offset(void)
{
    return s_col_offset;
}

esp_err_t ssd1306_flush(const uint8_t *fb, uint8_t page_mask)
{
    if (!s_dev || !fb) {
        return ESP_ERR_INVALID_STATE;
    }
    s_col_buf[0] = CTRL_DATA;

    for (int page = 0; page < 8; page++) {
        if ((page_mask & (1u << page)) == 0) {
            continue;
        }
        uint8_t col = s_col_offset;
        const uint8_t setup[3] = {0xB0 | (uint8_t)page, (uint8_t)(col & 0x0F),
                                  (uint8_t)(0x10 | (col >> 4))};
        if (cmd_list(setup, 3) != ESP_OK) {
            return ESP_FAIL;
        }
        /* 行优先 -> page 竖直打包 */
        for (int x = 0; x < UI_W; x++) {
            uint8_t v = 0;
            int byte_idx = x >> 3;
            uint8_t bit = (uint8_t)(1u << (x & 7));
            for (int r = 0; r < 8; r++) {
                if (fb[(page * 8 + r) * UI_STRIDE + byte_idx] & bit) {
                    v |= (uint8_t)(1u << r);
                }
            }
            s_col_buf[1 + x] = v;
        }
        if (wr(s_col_buf, sizeof(s_col_buf)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

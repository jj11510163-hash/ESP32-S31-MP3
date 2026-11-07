#include "lcd_panel.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

/* WKS35HV031-WCT / ESP32-S31 CB V1.1, per the board's 23_spilcd example. */
#define PIN_SCLK 16
#define PIN_MOSI 17
#define PIN_MISO 14
#define PIN_CS   15
#define PIN_DC   10
#define PIN_RST  60
#define PIN_BL   38
#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_DUTY_BITS LEDC_TIMER_10_BIT

/* 16 rows = 15 KB, small enough for an internal DMA-capable buffer. The screen
 * grid is fixed so the frame-diff bitmap stays valid between partial updates. */
#define TILE_ROWS 16
#define BAND_COUNT ((LCD_H + TILE_ROWS - 1) / TILE_ROWS)

/* The old 15 MHz value came from the module's 8080 write-cycle table (66 ns),
 * not from its 4-wire SPI timing table; ST7796 modules normally run much
 * faster over SPI. If the panel shows shifted pixels or noise, fall back to
 * 30, 20 or 15 MHz. */
#ifndef LCD_SPI_CLK_HZ
#define LCD_SPI_CLK_HZ (30 * 1000 * 1000)
#endif

static const char *TAG = "lcd_panel";
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static uint8_t *s_dma;
static uint16_t *s_sent;          /* PSRAM copy of what the panel already shows */
static uint32_t s_sent_valid;     /* one bit per 16-row band */
static bool s_ready;
static bool s_pwm_ready;
static bool s_backlight_on;
static uint8_t s_brightness = 3;
static const uint32_t s_bl_duty[5] = {153, 307, 512, 768, 1023};

static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                       void *ctx)
{
    (void)io;
    (void)event;
    (void)ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &wake);
    return wake == pdTRUE;
}

static esp_err_t cmd(uint8_t op, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, op, data, len);
}

/* True when the rectangle slice of this band already matches the panel GRAM.
 * Bands that were never sent in full stay invalid, so a partial first flush
 * can never make the driver skip pixels that were never transferred. */
static bool band_unchanged(const uint16_t *frame, int x, int top, int rows, int w, int band)
{
    if (!s_sent || band < 0 || band >= BAND_COUNT) return false;
    if (!(s_sent_valid & (1U << band))) return false;
    const uint16_t *cur = frame + (size_t)top * LCD_W + x;
    const uint16_t *old = s_sent + (size_t)top * LCD_W + x;
    if (x == 0 && w == LCD_W) {
        return memcmp(cur, old, (size_t)rows * LCD_W * sizeof(uint16_t)) == 0;
    }
    for (int r = 0; r < rows; r++) {
        if (memcmp(cur + (size_t)r * LCD_W, old + (size_t)r * LCD_W,
                   (size_t)w * sizeof(uint16_t)) != 0) return false;
    }
    return true;
}

static void band_commit(const uint16_t *frame, int x, int top, int rows, int w, int band)
{
    if (!s_sent || band < 0 || band >= BAND_COUNT) return;
    const uint16_t *cur = frame + (size_t)top * LCD_W + x;
    uint16_t *old = s_sent + (size_t)top * LCD_W + x;
    if (x == 0 && w == LCD_W) {
        memcpy(old, cur, (size_t)rows * LCD_W * sizeof(uint16_t));
    } else {
        for (int r = 0; r < rows; r++) {
            memcpy(old + (size_t)r * LCD_W, cur + (size_t)r * LCD_W,
                   (size_t)w * sizeof(uint16_t));
        }
    }
    if (x == 0 && w == LCD_W && top == band * TILE_ROWS && rows == TILE_ROWS) {
        s_sent_valid |= 1U << band;
    }
}

/* RGB565 in the framebuffer is a host-order uint16_t; the panel wants the high
 * byte first, so swap once while filling the DMA buffer. */
static void convert_band(const uint16_t *frame, int x, int top, int rows, int w)
{
    for (int r = 0; r < rows; r++) {
        const uint16_t *src = frame + (size_t)(top + r) * LCD_W + x;
        uint16_t *dst = (uint16_t *)(s_dma + (size_t)r * w * sizeof(uint16_t));
        for (int c = 0; c < w; c++) dst[c] = __builtin_bswap16(src[c]);
    }
}

static esp_err_t set_window(int x, int top, int w, int rows)
{
    const int right = x + w - 1;
    const int bottom = top + rows - 1;
    const uint8_t columns[] = {x >> 8, x & 0xFF, right >> 8, right & 0xFF};
    const uint8_t pages[] = {top >> 8, top & 0xFF, bottom >> 8, bottom & 0xFF};
    esp_err_t err = cmd(0x2A, columns, sizeof(columns));
    if (err == ESP_OK) err = cmd(0x2B, pages, sizeof(pages));
    return err;
}

static void abort_flush(void)
{
    s_sent_valid = 0;
    /* The timed-out transfer may still complete and post the semaphore; drain
     * it so a later flush cannot consume that stale token as its own. */
    vTaskDelay(pdMS_TO_TICKS(20));
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {}
}

/* One GRAM window is opened per run of changed bands. The first band starts
 * with 0x2C; the rest continue with lcd_cmd == -1, so the panel's address
 * counter simply keeps incrementing and no per-tile window commands are sent. */
static esp_err_t flush_run(const uint16_t *frame, int x, int y, int w, int h,
                           int first_band, int last_band)
{
    int run_top = first_band * TILE_ROWS;
    if (run_top < y) run_top = y;
    int run_bottom = (last_band + 1) * TILE_ROWS;
    if (run_bottom > y + h) run_bottom = y + h;

    esp_err_t err = set_window(x, run_top, w, run_bottom - run_top);
    if (err != ESP_OK) return err;

    bool first = true;
    for (int band = first_band; band <= last_band; band++) {
        int top = band * TILE_ROWS;
        if (top < y) top = y;
        int bottom = (band + 1) * TILE_ROWS;
        if (bottom > y + h) bottom = y + h;
        const int rows = bottom - top;

        convert_band(frame, x, top, rows, w);
        err = esp_lcd_panel_io_tx_color(s_io, first ? 0x2C : -1, s_dma,
                                        (size_t)w * rows * sizeof(uint16_t));
        if (err != ESP_OK) {
            abort_flush();
            return err;
        }
        first = false;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "SPI color transfer timeout at row %d", top);
            abort_flush();
            return ESP_ERR_TIMEOUT;
        }
        band_commit(frame, x, top, rows, w, band);
    }
    return ESP_OK;
}

static void backlight_gpio_fallback(void)
{
    gpio_reset_pin(PIN_BL);
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, s_backlight_on ? 1 : 0);
}

static esp_err_t backlight_apply(void)
{
    uint32_t duty = s_backlight_on ? s_bl_duty[s_brightness] : 0;
    esp_err_t err = ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    if (err == ESP_OK) err = ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight PWM failed: %s; using GPIO", esp_err_to_name(err));
        s_pwm_ready = false;
        ledc_stop(BL_LEDC_MODE, BL_LEDC_CHANNEL, 0);
        backlight_gpio_fallback();
    }
    return err;
}

void lcd_panel_backlight(bool on)
{
    s_backlight_on = on;
    if (s_pwm_ready) {
        backlight_apply();
    } else {
        gpio_set_level(PIN_BL, on ? 1 : 0);
    }
}

bool lcd_panel_brightness_available(void)
{
    return s_pwm_ready;
}

esp_err_t lcd_panel_set_brightness(uint8_t level)
{
    if (level > 4) return ESP_ERR_INVALID_ARG;
    if (s_ready && !s_pwm_ready) return ESP_ERR_NOT_SUPPORTED;
    s_brightness = level;
    if (s_pwm_ready) return backlight_apply();
    return s_ready ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

static void backlight_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        ledc_channel_config_t channel = {
            .speed_mode = BL_LEDC_MODE,
            .channel = BL_LEDC_CHANNEL,
            .timer_sel = BL_LEDC_TIMER,
            .gpio_num = PIN_BL,
            .duty = 0,
            .hpoint = 0,
        };
        err = ledc_channel_config(&channel);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight PWM unavailable: %s; using GPIO", esp_err_to_name(err));
        backlight_gpio_fallback();
        return;
    }
    s_pwm_ready = true;
}

esp_err_t lcd_panel_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK) return err;
    lcd_panel_backlight(false);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * TILE_ROWS * 2 + 8,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    s_done = xSemaphoreCreateBinary();
    s_dma = heap_caps_malloc(LCD_W * TILE_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    /* The diff buffer only saves bandwidth, so a failed allocation degrades to
     * unconditional full-band sends instead of breaking the display. */
    s_sent = heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(uint16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sent_valid = 0;
    if (!s_sent) ESP_LOGW(TAG, "no PSRAM for frame diff; every band will be sent");
    if (!s_done || !s_dma) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .pclk_hz = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = color_done,
    };
    err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) return err;

    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Same power/gamma sequence as the supplied ST7796U example. */
#define SEND(op, ...) do { const uint8_t p[] = {__VA_ARGS__}; \
    err = cmd((op), p, sizeof(p)); if (err != ESP_OK) return err; } while (0)
    err = cmd(0x11, NULL, 0); if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    SEND(0x3A, 0x55);                  /* RGB565 */
    SEND(0xF0, 0xC3);
    SEND(0xF0, 0x96);
    SEND(0xB4, 0x01);
    SEND(0xB6, 0x0A, 0xA2);
    SEND(0xB7, 0xC6);
    SEND(0xB9, 0x02, 0xE0);
    SEND(0xC0, 0x80, 0x16);
    SEND(0xC1, 0x19);
    SEND(0xC2, 0xA7);
    SEND(0xC5, 0x16);
    SEND(0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);
    SEND(0xE0, 0xF0, 0x07, 0x0D, 0x04, 0x05, 0x14, 0x36, 0x54,
               0x4C, 0x38, 0x13, 0x14, 0x2E, 0x34);
    SEND(0xE1, 0xF0, 0x10, 0x14, 0x0E, 0x0C, 0x08, 0x35, 0x44,
               0x4C, 0x26, 0x10, 0x12, 0x2C, 0x32);
    SEND(0xF0, 0x3C);
    SEND(0xF0, 0x69);
    /* Landscape: MV + MY + BGR; same rotation as spilcd_display_dir(1). */
    SEND(0x36, 0xA8);
#undef SEND
    err = cmd(0x21, NULL, 0); if (err != ESP_OK) return err;
    err = cmd(0x29, NULL, 0); if (err != ESP_OK) return err;
    s_ready = true;
    backlight_pwm_init();
    ESP_LOGI(TAG, "ST7796U ready: 480x320, SPI2 %d MHz", LCD_SPI_CLK_HZ / 1000000);
    return ESP_OK;
}

bool lcd_panel_present(void)
{
    return s_ready;
}

esp_err_t lcd_panel_flush_rect(const uint16_t *rgb565, int x, int y, int w, int h)
{
    if (!s_ready || !rgb565) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > LCD_W - x || h > LCD_H - y)
        return ESP_ERR_INVALID_ARG;

    const int first_band = y / TILE_ROWS;
    const int last_band = (y + h - 1) / TILE_ROWS;
    int run_start = -1;

    for (int band = first_band; band <= last_band; band++) {
        int top = band * TILE_ROWS;
        if (top < y) top = y;
        int bottom = (band + 1) * TILE_ROWS;
        if (bottom > y + h) bottom = y + h;
        const int rows = bottom - top;

        const bool changed = !band_unchanged(rgb565, x, top, rows, w, band);
        if (changed && run_start < 0) run_start = band;
        if (run_start >= 0 && (!changed || band == last_band)) {
            const int run_end = changed ? band : band - 1;
            esp_err_t err = flush_run(rgb565, x, y, w, h, run_start, run_end);
            if (err != ESP_OK) return err;
            run_start = -1;
        }
    }
    return ESP_OK;
}

esp_err_t lcd_panel_flush(const uint16_t *rgb565)
{
    return lcd_panel_flush_rect(rgb565, 0, 0, LCD_W, LCD_H);
}

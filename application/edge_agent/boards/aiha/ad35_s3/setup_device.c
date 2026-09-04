/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_aw9523b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gen_board_device_custom.h"

static const char *TAG = "ad35_s3";

#define AW9523_PIN_LCD_LED_0 8
#define AW9523_PIN_LCD_LED_1 9
#define AW9523_PIN_LCD_LED_2 10
#define AW9523_PIN_LCD_LED_3 11
#define AW9523_PIN_LCD_RESET 14

#define LCD_PIN_DC 45
#define LCD_PIN_WR 10
#define LCD_PIN_D0 9
#define LCD_PIN_D1 4
#define LCD_PIN_D2 3
#define LCD_PIN_D3 8
#define LCD_PIN_D4 18
#define LCD_PIN_D5 17
#define LCD_PIN_D6 16
#define LCD_PIN_D7 15

#define LCD_H_RES 480
#define LCD_V_RES 320
#define LCD_PIXEL_CLK_HZ (10 * 1000 * 1000)
#define LCD_MAX_TRANSFER_BYTES (LCD_H_RES * 40 * sizeof(uint16_t))

/* Built-in 8x8 ASCII font (public domain).
 * Each glyph is 8 bytes, LSB-first within each row. */
static const uint8_t s_font8x8_basic[128][8] = {
    [0x20] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    [0x2D] = {0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00}, /* - */
    [0x2E] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30}, /* . */
    [0x30] = {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    [0x31] = {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    [0x32] = {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    [0x33] = {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    [0x34] = {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    [0x35] = {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    [0x41] = {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    [0x43] = {0x1E, 0x33, 0x03, 0x03, 0x03, 0x33, 0x1E, 0x00}, /* C */
    [0x44] = {0x1F, 0x33, 0x33, 0x33, 0x33, 0x33, 0x1F, 0x00}, /* D */
    [0x46] = {0x3F, 0x33, 0x03, 0x1F, 0x03, 0x03, 0x03, 0x00}, /* F */
    [0x47] = {0x1E, 0x33, 0x03, 0x03, 0x3B, 0x33, 0x3E, 0x00}, /* G */
    [0x48] = {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    [0x4B] = {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    [0x4C] = {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x3F, 0x00}, /* L */
    [0x4E] = {0x33, 0x37, 0x3F, 0x3B, 0x33, 0x33, 0x33, 0x00}, /* N */
    [0x4F] = {0x1E, 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x00}, /* O */
    [0x50] = {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, /* P */
    [0x53] = {0x1E, 0x33, 0x03, 0x1E, 0x30, 0x33, 0x1E, 0x00}, /* S */
    [0x57] = {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    [0x2A] = {0x00, 0x36, 0x1C, 0x7F, 0x1C, 0x36, 0x00, 0x00}, /* * */
    [0x21] = {0x0C, 0x1E, 0x1E, 0x0C, 0x0C, 0x00, 0x0C, 0x00}, /* ! */
};

static esp_lcd_i80_bus_handle_t s_i80_bus;
static dev_display_lcd_handles_t s_lcd_handles;
static uint8_t s_i2c_diagnostics_handle;

static int i2c_diagnostics_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "I2C diagnostics device_handle is NULL");

    void *periph_handle = NULL;
    esp_err_t ret = esp_board_periph_ref_handle("i2c_master", &periph_handle);
    ESP_RETURN_ON_FALSE(ret == ESP_OK && periph_handle != NULL,
                        ret == ESP_OK ? ESP_FAIL : ret, TAG,
                        "I2C diagnostics could not get i2c_master");

    ESP_LOGI(TAG, "I2C scan start: SDA=GPIO5 SCL=GPIO4");
    ESP_LOGI(TAG, "I2C idle levels: SDA=%d SCL=%d",
             gpio_get_level(GPIO_NUM_5), gpio_get_level(GPIO_NUM_4));

    unsigned int found = 0;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)periph_handle;
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "I2C ACK at 7-bit address 0x%02x", (unsigned int)addr);
            ++found;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %u device(s)", found);

    esp_board_periph_unref_handle("i2c_master");
    *device_handle = &s_i2c_diagnostics_handle;
    return ESP_OK;
}

static int i2c_diagnostics_deinit(void *device_handle)
{
    (void)device_handle;
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(i2c_diagnostics, i2c_diagnostics_init, i2c_diagnostics_deinit);

static const dev_display_lcd_config_t s_lcd_config = {
    .name = "display_lcd",
    .chip = "st7796",
    .sub_type = "i80",
    .lcd_width = LCD_H_RES,
    .lcd_height = LCD_V_RES,
    .swap_xy = 1,
    .mirror_x = 1,
    .mirror_y = 0,
    .need_reset = 0,
    .invert_color = 1,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
};

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_aw9523b(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AW9523 creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* P0 uses push-pull outputs; both ports must be GPIO rather than LED mode. */
    uint8_t data = 0x10;
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_GCR, &data, 1),
                        TAG, "AW9523 push-pull configuration failed");
    data = 0xFF;
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_LEDMODE0, &data, 1),
                        TAG, "AW9523 P0 GPIO-mode configuration failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_LEDMODE1, &data, 1),
                        TAG, "AW9523 P1 GPIO-mode configuration failed");
    return ESP_OK;
}

static void cleanup_display_lcd(esp_lcd_panel_handle_t panel_handle,
                                esp_lcd_panel_io_handle_t io_handle)
{
    if (panel_handle != NULL) {
        esp_lcd_panel_del(panel_handle);
    }
    if (io_handle != NULL) {
        esp_lcd_panel_io_del(io_handle);
    }
    if (s_i80_bus != NULL) {
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
    }
}

/* Draw a single 8x8 glyph, scaled by `scale`, into an RGB565 framebuffer. */
static void draw_glyph_rgb565(uint16_t *fb, int fb_w, int fb_h,
                              int x, int y, char ch, int scale,
                              uint16_t color, uint16_t bg, bool with_bg)
{
    if (ch < 0 || ch >= 128) {
        return;
    }
    const uint8_t *glyph = s_font8x8_basic[(int)ch];
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            bool on = (bits >> col) & 0x1;
            if (!on && !with_bg) {
                continue;
            }
            uint16_t px = on ? color : bg;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    int px_x = x + col * scale + dx;
                    int px_y = y + row * scale + dy;
                    if (px_x < 0 || px_y < 0 || px_x >= fb_w || px_y >= fb_h) {
                        continue;
                    }
                    fb[px_y * fb_w + px_x] = px;
                }
            }
        }
    }
}

static void draw_string_rgb565(uint16_t *fb, int fb_w, int fb_h,
                               int x, int y, const char *str, int scale,
                               uint16_t color, uint16_t bg, bool with_bg)
{
    int cursor = x;
    for (const char *p = str; *p; ++p) {
        draw_glyph_rgb565(fb, fb_w, fb_h, cursor, y, *p, scale, color, bg, with_bg);
        cursor += 8 * scale + scale; /* 1-column spacing */
    }
}

/* Render a diagnostic screen: color bars + big "AD35-S3 OK" text.
 * Uses the panel handle directly (no display_service, no Lua). */
static esp_err_t render_boot_diagnostic(esp_lcd_panel_handle_t panel)
{
    /* After swap_xy + mirror_x the visible resolution is 480 wide x 320 tall. */
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    const size_t fb_bytes = (size_t)W * H * sizeof(uint16_t);

    uint16_t *fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "boot diag: framebuffer alloc failed (%u bytes)", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }

    /* Solid navy blue background (RGB565 big-endian sent by driver). */
    const uint16_t BG    = 0x001F; /* blue-ish */
    const uint16_t WHITE = 0xFFFF;
    const uint16_t RED   = 0xF800;
    const uint16_t GREEN = 0x07E0;
    const uint16_t YELLOW= 0xFFE0;

    /* Fill background. */
    for (int i = 0; i < W * H; ++i) {
        fb[i] = BG;
    }

    /* Four corner color blocks so orientation is obvious. */
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 60; ++x) {
            fb[y * W + x] = RED;                     /* top-left */
            fb[y * W + (W - 1 - x)] = GREEN;         /* top-right */
            fb[(H - 1 - y) * W + x] = YELLOW;        /* bottom-left */
            fb[(H - 1 - y) * W + (W - 1 - x)] = WHITE; /* bottom-right */
        }
    }

    /* Big title: "AD35-S3 OK" — scale=6 → 48x48 per glyph. */
    draw_string_rgb565(fb, W, H, 40, 100, "AD35-S3 OK", 6, WHITE, BG, false);
    /* Version line: scale=3 → 24x24 per glyph. */
    draw_string_rgb565(fb, W, H, 40, 200, "FW 0.1.5", 3, YELLOW, BG, false);

    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "boot diag: draw_bitmap failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "boot diag: rendered %dx%d framebuffer OK", W, H);
    }
    heap_caps_free(fb);
    return ret;
}

static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_io_expander_handle_t *expander = NULL;
    ESP_RETURN_ON_ERROR(esp_board_device_get_handle("gpio_expander", (void **)&expander),
                        TAG, "AW9523 handle unavailable");

    /* Assert LCD RESET low, then release. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(
                            *expander, 1U << AW9523_PIN_LCD_RESET, 0),
                        TAG, "LCD reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(
                            *expander, 1U << AW9523_PIN_LCD_RESET, 1),
                        TAG, "LCD reset release failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .data_gpio_nums = {
            LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
            LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_MAX_TRANSFER_BYTES,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &s_i80_bus), TAG,
                        "I80 bus creation failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.swap_color_bytes = 1,
    };
    esp_err_t ret = esp_lcd_new_panel_io_i80(s_i80_bus, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, NULL);
        ESP_LOGE(TAG, "I80 panel IO creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7796(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, io_handle);
        ESP_LOGE(TAG, "ST7796 panel creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_init(panel_handle);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_swap_xy(panel_handle, true);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_mirror(panel_handle, true, false);
    }
    if (ret == ESP_OK) {
        /* AD35-S3 uses an IPS variant of ST7796 (per moononournation
         * Dev_Device_Pins reference). IPS panels require CMD_INVON so the
         * pixels display in their intended polarity — without this, whites
         * appear near-black and text is invisible even when drawn. */
        ret = esp_lcd_panel_invert_color(panel_handle, true);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    }
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "ST7796 initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Enable backlight: AW9523 P8-P11 drive the LED cathodes.
     * These pins boot LOW (LEDs off); pull them HIGH so the panel is lit. */
    esp_err_t bl_ret = esp_io_expander_set_level(
        *expander,
        (1U << AW9523_PIN_LCD_LED_0) | (1U << AW9523_PIN_LCD_LED_1) |
            (1U << AW9523_PIN_LCD_LED_2) | (1U << AW9523_PIN_LCD_LED_3),
        1);
    if (bl_ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed: %s", esp_err_to_name(bl_ret));
    } else {
        ESP_LOGI(TAG, "backlight ON (AW9523 P8-P11 HIGH)");
    }

    /* Immediately render a boot diagnostic frame so we get visible proof of
     * end-to-end panel operation before any higher-level UI runs. */
    (void)render_boot_diagnostic(panel_handle);

    ret = esp_board_device_override_config("display_lcd", (void *)&s_lcd_config,
                                           sizeof(s_lcd_config));
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "Display configuration override failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd_handles.io_handle = io_handle;
    s_lcd_handles.panel_handle = panel_handle;
    *device_handle = &s_lcd_handles;
    ESP_LOGI(TAG, "AD35-S3 ST7796 I80 LCD ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)device_handle;
    cleanup_display_lcd(handles != NULL ? handles->panel_handle : NULL,
                        handles != NULL ? handles->io_handle : NULL);
    memset(&s_lcd_handles, 0, sizeof(s_lcd_handles));
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);

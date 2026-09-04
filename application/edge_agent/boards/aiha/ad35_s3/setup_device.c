/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
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
    .invert_color = 0,
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

static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_io_expander_handle_t *expander = NULL;
    ESP_RETURN_ON_ERROR(esp_board_device_get_handle("gpio_expander", (void **)&expander),
                        TAG, "AW9523 handle unavailable");

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
        ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    }
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "ST7796 initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

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

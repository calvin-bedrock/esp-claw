/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the "love" Lua module.
 */
esp_err_t lua_module_love2d_register(void);

/**
 * @brief Start the Love2D runtime task.
 *
 * Creates a persistent Lua state, loads love module and main.lua,
 * then enters the game loop at ~60 FPS with touch polling.
 *
 * @param display_panel_handle esp_lcd_panel_handle_t for the LCD.
 * @param touch_i2c_bus_handle i2c_master_bus_handle_t for FT6336U touch.
 * @return ESP_OK on success.
 */
esp_err_t lua_module_love2d_start(void *display_panel_handle, void *touch_i2c_bus_handle);

/**
 * @brief Set the esp_codec_dev output handle for love.audio.
 */
void love_set_codec_handle(void *codec_dev_handle);

#ifdef __cplusplus
}
#endif
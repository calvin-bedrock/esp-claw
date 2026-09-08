/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal header for Love2D graphics framebuffer management.
 * Declares functions shared between love_graphics.c and love_event.c.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate the offscreen RGB565 framebuffer in PSRAM.
 */
esp_err_t love_gfx_init_framebuffer(void);

/**
 * @brief Flush the dirty framebuffer to the LCD panel via draw_bitmap.
 */
void love_gfx_flush(void);

#ifdef __cplusplus
}
#endif
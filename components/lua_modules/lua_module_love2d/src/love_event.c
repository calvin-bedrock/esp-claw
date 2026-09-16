/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 *
 * Love2D runtime event loop.
 *
 * Dedicated FreeRTOS task that:
 * 1. Creates a persistent Lua state
 * 2. Requires the `love` module
 * 3. Runs /spiffs/main.lua (defines love.load, love.update, love.draw)
 * 4. Enters the game loop: calls love.update(dt), love.draw(), handles touch
 * 5. Flushes framebuffer to LCD after each draw()
 */

#include "lua_module_love2d.h"

#include <sys/stat.h>
#include <string.h>
#include <time.h>

#include "cap_lua.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "love_gfx_impl.h"  /* love_gfx_init_framebuffer, love_gfx_flush */

/* Forward declaration from lua_module_love2d.c — luaopen function for love. */
extern int luaopen_love(lua_State *L);

static const char *TAG = "love_event";

/* Touch chip FT6336U is at I2C 0x38, compatible with FT5x06 register layout. */
#define FT6336U_ADDR        0x38
#define FT6336U_TD_STATUS   0x02  /* Number of touch points */
#define FT6336U_TOUCH1_XH   0x03  /* Touch 1 X high byte */
#define FT6336U_TOUCH1_XL   0x04  /* Touch 1 X low byte */
#define FT6336U_TOUCH1_YH   0x05  /* Touch 1 Y high byte */
#define FT6336U_TOUCH1_YL   0x06  /* Touch 1 Y low byte */

/* Handles set by lua_module_love2d_start(). */
static void *s_panel = NULL;
static i2c_master_bus_handle_t s_touch_bus = NULL;
static bool s_touch_handling_enabled = false;

/* Lua reference indices for love.update / love.draw callbacks. */
static int s_love_update_ref = LUA_NOREF;
static int s_love_draw_ref   = LUA_NOREF;

/* Touch state tracking. */
static bool s_touch_prev_pressed = false;
static uint16_t s_touch_prev_x = 0;
static uint16_t s_touch_prev_y = 0;
static int s_touch_id_counter = 0;

/* ---- FT6336U I2C register helpers ---- */

static esp_err_t ft6336u_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_touch_bus == NULL) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT6336U_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_master_bus_add_device(s_touch_bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;

    ret = i2c_master_transmit(dev, &reg, 1, 20);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev, val, 1, 20);
    }
    i2c_master_bus_rm_device(dev);
    return ret;
}

static esp_err_t ft6336u_read_touch(bool *pressed, uint16_t *x, uint16_t *y)
{
    uint8_t td_status = 0;
    esp_err_t ret = ft6336u_read_reg(FT6336U_TD_STATUS, &td_status);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t points = td_status & 0x0F;
    if (points == 0) {
        *pressed = false;
        *x = 0;
        *y = 0;
        return ESP_OK;
    }

    /* Read touch point 1 X (3 bytes: XH, XL, YH, YL). */
    uint8_t xh = 0, xl = 0, yh = 0, yl = 0;
    ret = ft6336u_read_reg(FT6336U_TOUCH1_XH, &xh);
    if (ret == ESP_OK) ret = ft6336u_read_reg(FT6336U_TOUCH1_XL, &xl);
    if (ret == ESP_OK) ret = ft6336u_read_reg(FT6336U_TOUCH1_YH, &yh);
    if (ret == ESP_OK) ret = ft6336u_read_reg(FT6336U_TOUCH1_YL, &yl);
    if (ret != ESP_OK) return ret;

    *x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    *y = (uint16_t)(((yh & 0x0F) << 8) | yl);
    *pressed = true;
    return ESP_OK;
}

/* ---- Trigger Lua touch callbacks ---- */

static void trigger_touch_event(lua_State *L, const char *fn_name,
                                 int id, int x, int y, int dx, int dy)
{
    if (L == NULL) return;

    lua_getglobal(L, fn_name);
    if (lua_isfunction(L, -1)) {
        lua_pushinteger(L, id);
        lua_pushinteger(L, x);
        lua_pushinteger(L, y);
        lua_pushinteger(L, dx);
        lua_pushinteger(L, dy);
        if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "error in %s: %s", fn_name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

/* ---- The Love2D runtime task ---- */

static void love_runtime_task(void *arg)
{
    (void)arg;
    esp_err_t ret;
    const char *script_paths[] = {
        "/spiffs/main.lua",
        "/system/scripts/builtin/main.lua",
    };

    /* Wait for system initialization to complete before starting
     * the Love2D loop.  This lets WiFi, HTTP server, and other
     * board services finish initializing so the GDMA link isn't
     * starved by the 60 FPS frame loop. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 1. Allocate framebuffer */
    ret = love_gfx_init_framebuffer();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "framebuffer init failed, aborting");
        vTaskDelete(NULL);
        return;
    }

    /* 2. Create Lua state */
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        ESP_LOGE(TAG, "failed to create Lua state");
        vTaskDelete(NULL);
        return;
    }
    luaL_openlibs(L);

    /* 3. Load love module directly via luaL_requiref.
     * The module was statically linked by the linker (same component),
     * so we call luaopen_love directly instead of going through require
     * which would need cap_lua's module registry. */
    luaL_requiref(L, "love", luaopen_love, 1);
    lua_pop(L, 1);  /* remove copy left by requiref */

    /* 4. Add script paths to package.path */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *cur_path = lua_tostring(L, -1);
    const char *extra = ";/spiffs/?.lua;/spiffs/?/init.lua;/system/scripts/builtin/?.lua";
    size_t new_len = strlen(cur_path) + strlen(extra) + 1;
    char *new_path = malloc(new_len);
    if (new_path) {
        snprintf(new_path, new_len, "%s%s", cur_path, extra);
        lua_pop(L, 1); /* pop old path */
        lua_pushstring(L, new_path);
        lua_setfield(L, -2, "path");
        free(new_path);
    } else {
        lua_pop(L, 1); /* pop path */
    }
    lua_pop(L, 1); /* pop package */

    /* 5. Load and execute ONLY /spiffs/main.lua — no builtin demo fallback */
    struct stat st;
    if (stat("/spiffs/main.lua", &st) != 0 || st.st_size == 0) {
        ESP_LOGI(TAG, "No custom Lua script at /spiffs/main.lua. Idle — no demo.");
        vTaskDelete(NULL);
        return;
    }
    if (luaL_dofile(L, "/spiffs/main.lua") != LUA_OK) {
        ESP_LOGE(TAG, "Fatal: /spiffs/main.lua load error: %s. Suspending.", lua_tostring(L, -1));
        lua_pop(L, 1);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "loaded: /spiffs/main.lua");

    /* 6. Call love.load() */
    lua_getglobal(L, "love");
    lua_getfield(L, -1, "load");
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "love.load error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* pop love table */

    /* Capture update/draw references. */
    lua_getglobal(L, "love");
    lua_getfield(L, -1, "update");
    if (lua_isfunction(L, -1)) {
        s_love_update_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        s_love_update_ref = LUA_NOREF;
    }
    lua_getfield(L, -1, "draw");
    if (lua_isfunction(L, -1)) {
        s_love_draw_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        s_love_draw_ref = LUA_NOREF;
    }
    lua_pop(L, 1); /* pop love table */

    /* Reset touch state. */
    s_touch_prev_pressed = false;
    s_touch_prev_x = 0;
    s_touch_prev_y = 0;

    /* 7. Main game loop */
    int64_t last_tick = esp_timer_get_time();
    const int64_t target_frame_us = 33333;  /* ~30 FPS to keep GDMA healthy */

    while (1) {
        int64_t now = esp_timer_get_time();
        int64_t frame_us = now - last_tick;
        last_tick = now;
        if (frame_us > 100000) {
            frame_us = 0;
        }

        float dt = (float)frame_us / 1000000.0f;
        if (dt > 0.1f) dt = 0.1f;

        /* ---- Touch polling ---- */
        if (s_touch_handling_enabled && s_touch_bus != NULL) {
            bool pressed = false;
            uint16_t tx = 0, ty = 0;
            if (ft6336u_read_touch(&pressed, &tx, &ty) == ESP_OK) {
                uint16_t disp_x = (tx * 480) / 320;
                uint16_t disp_y = (ty * 320) / 480;
                if (disp_x >= 480) disp_x = 479;
                if (disp_y >= 320) disp_y = 319;

                int dx = 0, dy = 0;
                if (pressed) {
                    if (s_touch_prev_pressed) {
                        dx = (int)disp_x - (int)s_touch_prev_x;
                        dy = (int)disp_y - (int)s_touch_prev_y;
                        if (dx != 0 || dy != 0) {
                            trigger_touch_event(L, "love.touchmoved",
                                s_touch_id_counter, disp_x, disp_y, dx, dy);
                        }
                    } else {
                        s_touch_id_counter++;
                        trigger_touch_event(L, "love.touchpressed",
                            s_touch_id_counter, disp_x, disp_y, 0, 0);
                    }
                } else if (s_touch_prev_pressed) {
                    trigger_touch_event(L, "love.touchreleased",
                        s_touch_id_counter, s_touch_prev_x, s_touch_prev_y, 0, 0);
                }
                s_touch_prev_pressed = pressed;
                s_touch_prev_x = disp_x;
                s_touch_prev_y = disp_y;
            }
        }

        /* ---- love.update(dt) ---- */
        if (s_love_update_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_love_update_ref);
            lua_pushnumber(L, (lua_Number)dt);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                ESP_LOGE(TAG, "Fatal love.update error: %s. Suspending!", lua_tostring(L, -1));
                lua_pop(L, 1);
                vTaskSuspend(NULL);
                return;
            }
        }

        /* ---- love.draw() ---- */
        if (s_love_draw_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_love_draw_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                ESP_LOGE(TAG, "Fatal love.draw error: %s. Suspending!", lua_tostring(L, -1));
                lua_pop(L, 1);
                vTaskSuspend(NULL);
                return;
            }
        }

        /* Flush framebuffer to LCD. */
        /* Disabled: user explicitly requested no LCD drawing / no screen refresh.
         * The framebuffer stays untouched; GDMA is not used.
         * love_gfx_flush(); */

        /* Frame rate limiting. */
        int64_t elapsed = esp_timer_get_time() - now;
        if (elapsed < target_frame_us) {
            vTaskDelay(pdMS_TO_TICKS((target_frame_us - elapsed) / 1000));
        } else {
            taskYIELD();
        }
    }

    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t lua_module_love2d_start(void *display_panel_handle, void *touch_i2c_bus_handle)
{
    s_panel = display_panel_handle;
    s_touch_bus = (i2c_master_bus_handle_t)touch_i2c_bus_handle;
    s_touch_handling_enabled = (touch_i2c_bus_handle != NULL);

    /* Pass panel handle to the graphics module. */
    extern void love_set_display_handle(void *panel_handle);
    love_set_display_handle(display_panel_handle);

    /* Create the Love2D runtime task. */
    BaseType_t ok = xTaskCreate(love_runtime_task, "love2d", 8192, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create love2d task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Love2D runtime task started");
    return ESP_OK;
}
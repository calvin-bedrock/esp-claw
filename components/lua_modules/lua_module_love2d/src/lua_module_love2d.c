/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 *
 * Love2D-compatible Lua API binding for ESP-Claw / AD35-S3.
 *
 * Registers the global `love` table with sub-tables:
 *   love.graphics  — clear, rectangle, circle, print, draw
 *   love.audio     — beep, play, set_volume
 *
 * The runtime event loop fires Lua callbacks:
 *   love.update(dt), love.draw()
 *   love.touchpressed(id, x, y, dx, dy)
 *   love.touchreleased(id, x, y)
 *   love.touchmoved(id, x, y, dx, dy)
 *   love.voice_wake(energy)
 */

#include "lua_module_love2d.h"

#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"

static const char *TAG = "love2d";

/* ---- Forward declarations from love_graphics.c ---- */
extern void love_set_display_handle(void *panel_handle);
extern int lua_love_graphics_clear(lua_State *L);
extern int lua_love_graphics_rectangle(lua_State *L);
extern int lua_love_graphics_circle(lua_State *L);
extern int lua_love_graphics_print(lua_State *L);
extern int lua_love_graphics_draw(lua_State *L);

/* ---- Forward declarations from love_audio.c ---- */
extern int lua_love_audio_beep(lua_State *L);
extern int lua_love_audio_play(lua_State *L);
extern int lua_love_audio_set_volume(lua_State *L);

/* ---- Lua bindings ---- */

static int lua_love_getVersion(lua_State *L)
{
    lua_pushstring(L, "0.1.0");
    return 1;
}

static const luaL_Reg graphics_funcs[] = {
    {"clear",     lua_love_graphics_clear},
    {"rectangle", lua_love_graphics_rectangle},
    {"circle",    lua_love_graphics_circle},
    {"print",     lua_love_graphics_print},
    {"draw",      lua_love_graphics_draw},
    {NULL, NULL},
};

static const luaL_Reg audio_funcs[] = {
    {"beep",       lua_love_audio_beep},
    {"play",       lua_love_audio_play},
    {"set_volume", lua_love_audio_set_volume},
    {NULL, NULL},
};

int luaopen_love(lua_State *L)
{
    /* love.graphics sub-table */
    lua_newtable(L);
    luaL_setfuncs(L, graphics_funcs, 0);

    /* Set default drawing color to white */
    lua_pushinteger(L, 255); lua_setfield(L, -2, "fg_r");
    lua_pushinteger(L, 255); lua_setfield(L, -2, "fg_g");
    lua_pushinteger(L, 255); lua_setfield(L, -2, "fg_b");

    /* love.audio sub-table */
    lua_newtable(L);
    luaL_setfuncs(L, audio_funcs, 0);

    /* Main love table */
    static const luaL_Reg love_funcs[] = {
        {"getVersion", lua_love_getVersion},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, love_funcs, 0);

    lua_pushvalue(L, -2);  /* audio table */
    lua_setfield(L, -2, "audio");

    lua_pushvalue(L, -3);  /* graphics table (one deeper due to audio push) */
    lua_setfield(L, -2, "graphics");

    /* Clean up extra references */
    lua_remove(L, -2);  /* audio table */
    lua_remove(L, -2);  /* graphics table */

    return 1;
}

esp_err_t lua_module_love2d_register(void)
{
    return cap_lua_register_module("love", luaopen_love);
}
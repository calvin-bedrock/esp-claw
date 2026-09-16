#pragma once
#include <lua.h>
#include <lauxlib.h>

int luaopen_display_ui(lua_State *L);
void display_ui_init(void);  // Initialize framebuffer
int lua_gfx_clear(lua_State *L);      // clear(r,g,b)
int lua_gfx_print(lua_State *L);      // print(text, x, y)
int lua_gfx_pixel_cat(lua_State *L);  // draw_cat(x, y, state)

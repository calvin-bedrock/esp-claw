#include "lua_module_display_ui.h"
#include "cat_images.h"
#include <string.h>

#define DISPLAY_W 320
#define DISPLAY_H 480

// Framebuffer for display (RGB565)
static uint16_t s_fb[DISPLAY_W * DISPLAY_H];

// Initialize framebuffer to black
void display_ui_init(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

// Helper: set a pixel in framebuffer
static void fb_set_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) return;
    s_fb[y * DISPLAY_W + x] = color;
}

// Helper: draw a 16x16 sprite (from cat_images.h) at position (x,y)
// Assumes sprite is 16x16 array of uint16_t values
static void fb_draw_sprite(int x, int y, const uint16_t *sprite) {
    if (!sprite) return;
    
    // Clip to display bounds
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + 16 > DISPLAY_W ? DISPLAY_W : x + 16;
    int y1 = y + 16 > DISPLAY_H ? DISPLAY_H : y + 16;
    
    int sx0 = x0 - x;  // source x offset
    int sy0 = y0 - y;  // source y offset
    
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int sx = xx - x + sx0;
            int sy = yy - y + sy0;
            uint16_t pixel = sprite[sy * 16 + sx];
            fb_set_pixel(xx, yy, pixel);
        }
    }
}

// Lua API: clear(r,g,b)
int lua_gfx_clear(lua_State *L) {
    int r = luaL_checkinteger(L, 1);
    int g = luaL_checkinteger(L, 2);
    int b = luaL_checkinteger(L, 3);
    
    // Convert RGB888 to RGB565
    uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    
    // Fill framebuffer
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) {
        s_fb[i] = color;
    }
    
    // TODO: Actually push to display - for now just update framebuffer
    // In real implementation, this would call esp_lcd_panel_draw_bitmap
    
    return 0;
}

// Lua API: print(text, x, y) - simplified 8x8 font
int lua_gfx_print(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    
    // Very basic text rendering - just set a few pixels as placeholder
    // In real implementation, this would use a font bitmap
    if (text && *text) {
        // Draw first character as a simple block for demo
        uint16_t color = 0xFFFF; // White
        for (int dy = 0; dy < 8 && y + dy < DISPLAY_H; dy++) {
            for (int dx = 0; dx < 8 && x + dx < DISPLAY_W; dx++) {
                fb_set_pixel(x + dx, y + dy, color);
            }
        }
    }
    
    return 0;
}

// Lua API: draw_cat(state, x, y) - draws the pixel cat sprite for given state
// state: 0=idle, 1=eyes, 2=swipe, 3=wake, 4=shutter
int lua_gfx_pixel_cat(lua_State *L) {
    int state = luaL_checkinteger(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    
    // Validate state
    if (state < 0) state = 0;
    if (state > 4) state = 4;
    
    // Select the appropriate sprite array
    const uint16_t *sprite = NULL;
    switch(state) {
        case 0: sprite = cat_idle; break;
        case 1: sprite = cat_eyes; break;
        case 2: sprite = cat_swipe; break;
        case 3: sprite = cat_wake; break;
        case 4: sprite = cat_shutter; break;
        default: sprite = cat_idle; break;
    }
    
    if (sprite) {
        fb_draw_sprite(x, y, sprite);
    }
    
    return 0;
}

// Register Lua functions
int luaopen_display_ui(lua_State *L) {
    static const luaL_Reg display_ui_funcs[] = {
        {"clear", lua_gfx_clear},
        {"print", lua_gfx_print},
        {"draw_cat", lua_gfx_pixel_cat},
        {NULL, NULL}
    };
    
    luaL_newlib(L, display_ui_funcs);
    return 1;
}

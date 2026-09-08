idf_component_register(
    SRC_DIRS
        "src"
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        cap_lua
        display_service
        driver
        esp_timer
        esp_lcd
        esp_codec_dev
)

# lauxlib.h's luaL_dostring/luaL_dofile macros produce an unused
# ||-expression result that trips -Werror=unused-value on xtensa-gcc.
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-unused-value")
#pragma once
#include <lua.h>
#include <lauxlib.h>

typedef struct lua_script_manager_t *lua_script_manager_handle_t;

lua_script_manager_handle_t lua_script_manager_create(void);
void lua_script_manager_delete(lua_script_manager_handle_t handle);
int lua_script_upload(lua_State *L);   // upload(path, content)
int lua_script_reload(lua_State *L);    // reload(name)
int lua_script_execute(lua_State *L);   // execute(name, [params])
int lua_script_list(lua_State *L);      // list() -> table of scripts

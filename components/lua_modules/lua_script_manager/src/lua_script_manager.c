#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "lua_script_manager.h"
#include "claw_paths.h"

#define SCRIPT_DIR CLAW_PATH_DATA "/lua_scripts"
#define MAX_SCRIPTS 32
#define MAX_SCRIPT_SIZE 65536

static const char *TAG = "lua_script_mgr";

typedef struct {
    lua_State *L;
    char script_names[MAX_SCRIPTS][64];
    int script_count;
} lua_script_manager_t;

static esp_err_t ensure_script_dir(void) {
    // Use claw_paths for data partition
    const char *path = claw_paths_join(CLAW_PATH_DATA, "lua_scripts");
    if (path == NULL) {
        ESP_LOGE(TAG, "Failed to get script directory path");
        return ESP_FAIL;
    }
    
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    
    // Create directory
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create script directory: %s", strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Script directory ready: %s", path);
    return ESP_OK;
}

lua_script_manager_handle_t lua_script_manager_create(void) {
    lua_script_manager_t *mgr = calloc(1, sizeof(lua_script_manager_t));
    if (!mgr) return NULL;
    
    mgr->L = luaL_newstate();
    if (!mgr->L) {
        free(mgr);
        return NULL;
    }
    luaL_openlibs(mgr->L);
    
    if (ensure_script_dir() != ESP_OK) {
        lua_close(mgr->L);
        free(mgr);
        return NULL;
    }
    
    return mgr;
}

void lua_script_manager_delete(lua_script_manager_handle_t handle) {
    if (handle) {
        lua_script_manager_t *mgr = (lua_script_manager_t *)handle;
        if (mgr->L) lua_close(mgr->L);
        free(mgr);
    }
}

static const char *get_script_path(const char *name, char *buf, size_t bufsz) {
    const char *dir = claw_paths_join(CLAW_PATH_DATA, "lua_scripts");
    if (!dir) return NULL;
    snprintf(buf, bufsz, "%s/%s", dir, name);
    return buf;
}

int lua_script_upload(lua_State *L) {
    // upload(path, content)
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *content = luaL_checklstring(L, 2, &len);
    
    if (len > MAX_SCRIPT_SIZE) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Script too large");
        return 2;
    }
    
    char fullpath[256];
    if (!get_script_path(path, fullpath, sizeof(fullpath))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Invalid path");
        return 2;
    }
    
    FILE *f = fopen(fullpath, "w");
    if (!f) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    fwrite(content, 1, len, f);
    fclose(f);
    
    lua_pushboolean(L, 1);
    return 1;
}

int lua_script_reload(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    
    char fullpath[256];
    if (!get_script_path(name, fullpath, sizeof(fullpath))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Invalid name");
        return 2;
    }
    
    // For hot-reload, we just re-read the file on next execute
    // In a full implementation, we'd clear any cached module
    lua_pushboolean(L, 1);
    return 1;
}

int lua_script_execute(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    
    char fullpath[256];
    if (!get_script_path(name, fullpath, sizeof(fullpath))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Invalid name");
        return 2;
    }
    
    FILE *f = fopen(fullpath, "r");
    if (!f) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Script not found");
        return 2;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size > MAX_SCRIPT_SIZE) {
        fclose(f);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Script too large");
        return 2;
    }
    
    char *code = malloc(size + 1);
    if (!code) {
        fclose(f);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "OOM");
        return 2;
    }
    
    fread(code, 1, size, f);
    code[size] = '\0';
    fclose(f);
    
    // Execute the script in a new state (isolated)
    lua_State *exec_L = luaL_newstate();
    if (!exec_L) {
        free(code);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Failed to create Lua state");
        return 2;
    }
    luaL_openlibs(exec_L);
    
    // Register display UI functions
    extern int luaopen_display_ui(lua_State *L);
    luaopen_display_ui(exec_L);
    
    int err = luaL_loadstring(exec_L, code);
    if (err == LUA_OK) {
        err = lua_pcall(exec_L, 0, LUA_MULTRET, 0);
    }
    
    free(code);
    const char *err_msg = err ? lua_tostring(exec_L, -1) : "ok";
    lua_close(exec_L);
    
    lua_pushboolean(L, err == 0);
    lua_pushstring(L, err_msg);
    return 2;
}

int lua_script_list(lua_State *L) {
    const char *dir = claw_paths_join(CLAW_PATH_DATA, "lua_scripts");
    if (!dir) {
        lua_newtable(L);
        return 1;
    }
    
    DIR *d = opendir(dir);
    if (!d) {
        lua_newtable(L);
        return 1;
    }
    
    lua_newtable(L);
    int idx = 1;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".lua")) {
            lua_pushinteger(L, idx++);
            lua_pushstring(L, entry->d_name);
            lua_settable(L, -3);
        }
    }
    closedir(d);
    return 1;
}

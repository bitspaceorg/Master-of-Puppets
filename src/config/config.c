/*
 * Master of Puppets — Lua Config Loader
 * config.c — Parse Lua config files for viewport, camera, and keymap settings
 *
 * Requires: MOP_HAS_LUA (gated by MOP_ENABLE_LUA in Makefile)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_LUA)

#include <mop/mop.h>
#include "viewport/viewport_internal.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Config structure
 * ------------------------------------------------------------------------- */

#define MOP_MAX_KEYBINDS 64

struct MopConfig {
    /* Viewport */
    bool has_width, has_height;
    int  width, height;

    bool has_clear_color;
    MopColor clear_color;

    /* Camera */
    bool has_cam_distance;
    float cam_distance;

    bool has_cam_yaw;
    float cam_yaw;

    bool has_cam_pitch;
    float cam_pitch;

    bool has_cam_target;
    MopVec3 cam_target;

    bool has_cam_fov;
    float cam_fov;

    /* Keymap */
    struct { char key[32]; char action[32]; } keybinds[MOP_MAX_KEYBINDS];
    int keybind_count;
};

/* -------------------------------------------------------------------------
 * Lua table helpers
 * ------------------------------------------------------------------------- */

/* Read a number field from the table at the top of the stack.
 * Returns true if the field exists and is a number. */
static bool lua_get_number(lua_State *L, const char *field, double *out) {
    lua_getfield(L, -1, field);
    bool ok = lua_isnumber(L, -1);
    if (ok) *out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return ok;
}

/* Read a color array {r, g, b, a} from a field in the table at top of stack. */
static bool lua_get_color(lua_State *L, const char *field, MopColor *out) {
    lua_getfield(L, -1, field);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }

    float c[4] = {0, 0, 0, 1};
    for (int i = 0; i < 4; i++) {
        lua_rawgeti(L, -1, i + 1);
        if (lua_isnumber(L, -1))
            c[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    *out = (MopColor){c[0], c[1], c[2], c[3]};
    lua_pop(L, 1);
    return true;
}

/* Read a vec3 array {x, y, z} from a field in the table at top of stack. */
static bool lua_get_vec3(lua_State *L, const char *field, MopVec3 *out) {
    lua_getfield(L, -1, field);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }

    float v[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        lua_rawgeti(L, -1, i + 1);
        if (lua_isnumber(L, -1))
            v[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    *out = (MopVec3){v[0], v[1], v[2]};
    lua_pop(L, 1);
    return true;
}

/* -------------------------------------------------------------------------
 * Public API — Load
 * ------------------------------------------------------------------------- */

MopConfig *mop_config_load(const char *path) {
    if (!path) return NULL;

    lua_State *L = luaL_newstate();
    if (!L) return NULL;
    luaL_openlibs(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        lua_close(L);
        return NULL;
    }

    MopConfig *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { lua_close(L); return NULL; }

    /* ---- viewport table ---- */
    lua_getglobal(L, "viewport");
    if (lua_istable(L, -1)) {
        double v;
        if (lua_get_number(L, "width", &v))  { cfg->has_width = true; cfg->width = (int)v; }
        if (lua_get_number(L, "height", &v)) { cfg->has_height = true; cfg->height = (int)v; }
        cfg->has_clear_color = lua_get_color(L, "clear_color", &cfg->clear_color);
    }
    lua_pop(L, 1);

    /* ---- camera table ---- */
    lua_getglobal(L, "camera");
    if (lua_istable(L, -1)) {
        double v;
        if (lua_get_number(L, "distance", &v)) { cfg->has_cam_distance = true; cfg->cam_distance = (float)v; }
        if (lua_get_number(L, "yaw", &v))      { cfg->has_cam_yaw = true; cfg->cam_yaw = (float)v; }
        if (lua_get_number(L, "pitch", &v))    { cfg->has_cam_pitch = true; cfg->cam_pitch = (float)v; }
        if (lua_get_number(L, "fov", &v))      { cfg->has_cam_fov = true; cfg->cam_fov = (float)v; }
        cfg->has_cam_target = lua_get_vec3(L, "target", &cfg->cam_target);
    }
    lua_pop(L, 1);

    /* ---- keymap table ---- */
    lua_getglobal(L, "keymap");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) && cfg->keybind_count < MOP_MAX_KEYBINDS) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                const char *key = lua_tostring(L, -2);
                const char *act = lua_tostring(L, -1);
                int idx = cfg->keybind_count;
                strncpy(cfg->keybinds[idx].key, key, 31);
                cfg->keybinds[idx].key[31] = '\0';
                strncpy(cfg->keybinds[idx].action, act, 31);
                cfg->keybinds[idx].action[31] = '\0';
                cfg->keybind_count++;
            }
            lua_pop(L, 1);  /* pop value, keep key for next iteration */
        }
    }
    lua_pop(L, 1);

    lua_close(L);
    return cfg;
}

/* -------------------------------------------------------------------------
 * Public API — Free
 * ------------------------------------------------------------------------- */

void mop_config_free(MopConfig *cfg) {
    free(cfg);
}

/* -------------------------------------------------------------------------
 * Public API — Apply to viewport
 * ------------------------------------------------------------------------- */

void mop_config_apply(const MopConfig *cfg, MopViewport *vp) {
    if (!cfg || !vp) return;

    if (cfg->has_width && cfg->has_height)
        mop_viewport_resize(vp, cfg->width, cfg->height);

    if (cfg->has_clear_color)
        mop_viewport_set_clear_color(vp, cfg->clear_color);

    /* Camera settings — modify the viewport's owned camera */
    if (cfg->has_cam_distance)  vp->camera.distance   = cfg->cam_distance;
    if (cfg->has_cam_yaw)       vp->camera.yaw        = cfg->cam_yaw;
    if (cfg->has_cam_pitch)     vp->camera.pitch       = cfg->cam_pitch;
    if (cfg->has_cam_target)    vp->camera.target      = cfg->cam_target;
    if (cfg->has_cam_fov)       vp->camera.fov_degrees = cfg->cam_fov;
}

/* -------------------------------------------------------------------------
 * Public API — Keymap lookup
 * ------------------------------------------------------------------------- */

const char *mop_config_get_action(const MopConfig *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (int i = 0; i < cfg->keybind_count; i++) {
        if (strcmp(cfg->keybinds[i].key, key) == 0)
            return cfg->keybinds[i].action;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API — Resolve action string to MopInputType
 * ------------------------------------------------------------------------- */

int mop_config_resolve_input(const char *action) {
    if (!action) return -1;

    static const struct { const char *name; int type; } map[] = {
        { "translate",  MOP_INPUT_MODE_TRANSLATE    },
        { "rotate",     MOP_INPUT_MODE_ROTATE       },
        { "scale",      MOP_INPUT_MODE_SCALE        },
        { "wireframe",  MOP_INPUT_TOGGLE_WIREFRAME  },
        { "reset_view", MOP_INPUT_RESET_VIEW        },
        { "deselect",   MOP_INPUT_DESELECT          },
    };

    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (strcmp(action, map[i].name) == 0)
            return map[i].type;
    }
    return -1;
}

#endif /* MOP_HAS_LUA */

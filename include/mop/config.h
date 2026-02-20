/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * config.h — Lua-based configuration system
 *
 * Optional module gated by MOP_HAS_LUA.  When Lua is not available,
 * all functions are defined as inline stubs that return failure.
 *
 * The config file is a plain Lua script that sets global tables:
 *
 *   viewport = { width=960, height=720, clear_color={0.12,0.12,0.16,1} }
 *   camera   = { distance=4.5, yaw=0.6, pitch=0.4, target={0,0.4,0}, fov=60 }
 *   keymap   = { t="translate", g="rotate", e="scale", w="wireframe", ... }
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFIG_H
#define MOP_CONFIG_H

#include "types.h"

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopConfig   MopConfig;

#if defined(MOP_HAS_LUA)

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Load a Lua config file.  Returns NULL on failure (file not found,
 * parse error, out of memory).  The caller must free with mop_config_free. */
MopConfig *mop_config_load(const char *path);

/* Free config resources. */
void mop_config_free(MopConfig *cfg);

/* -------------------------------------------------------------------------
 * Apply config to viewport
 *
 * Sets viewport dimensions, clear color, camera parameters — only for
 * fields that were present in the config file.
 * ------------------------------------------------------------------------- */

void mop_config_apply(const MopConfig *cfg, MopViewport *vp);

/* -------------------------------------------------------------------------
 * Keymap lookup
 *
 * Given a platform-agnostic key name (lowercase letter, or "escape",
 * "space", "tab", etc.), returns the action string from the config's
 * keymap table.  Returns NULL if no binding exists for that key.
 *
 * Well-known action strings that map to MopInputType:
 *   "translate"  → MOP_INPUT_MODE_TRANSLATE
 *   "rotate"     → MOP_INPUT_MODE_ROTATE
 *   "scale"      → MOP_INPUT_MODE_SCALE
 *   "wireframe"  → MOP_INPUT_TOGGLE_WIREFRAME
 *   "reset_view" → MOP_INPUT_RESET_VIEW
 *   "deselect"   → MOP_INPUT_DESELECT
 *
 * Unknown strings are returned as-is for app-specific handling.
 * ------------------------------------------------------------------------- */

const char *mop_config_get_action(const MopConfig *cfg, const char *key);

/* -------------------------------------------------------------------------
 * Resolve action string to MopInputType
 *
 * Returns the MopInputType enum value for well-known action strings.
 * Returns -1 if the action is app-specific (not a built-in MOP input).
 * ------------------------------------------------------------------------- */

int mop_config_resolve_input(const char *action);

#else /* !MOP_HAS_LUA */

/* Stubs — config not available without Lua */
static inline MopConfig   *mop_config_load(const char *p) { (void)p; return 0; }
static inline void          mop_config_free(MopConfig *c)  { (void)c; }
static inline void          mop_config_apply(const MopConfig *c, MopViewport *v)
                                { (void)c; (void)v; }
static inline const char   *mop_config_get_action(const MopConfig *c, const char *k)
                                { (void)c; (void)k; return 0; }
static inline int           mop_config_resolve_input(const char *a) { (void)a; return -1; }

#endif /* MOP_HAS_LUA */

#endif /* MOP_CONFIG_H */

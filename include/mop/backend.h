/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * backend.h — Backend selection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_BACKEND_H
#define MOP_BACKEND_H

/* -------------------------------------------------------------------------
 * Backend type enumeration
 *
 * MOP_BACKEND_AUTO selects the best available backend for the current
 * platform.  The selection order is: Vulkan > OpenGL > CPU.
 *
 * MOP_BACKEND_CPU is always available.
 * GPU backends are available only when the corresponding driver is present.
 * ------------------------------------------------------------------------- */

typedef enum MopBackendType {
    MOP_BACKEND_AUTO   = 0,
    MOP_BACKEND_CPU    = 1,
    MOP_BACKEND_OPENGL = 2,
    MOP_BACKEND_VULKAN = 3,
    MOP_BACKEND_COUNT  = 4
} MopBackendType;

/* Return a human-readable name for the backend. Never returns NULL. */
const char *mop_backend_name(MopBackendType type);

/* Return the default backend for the current platform. */
MopBackendType mop_backend_default(void);

#endif /* MOP_BACKEND_H */

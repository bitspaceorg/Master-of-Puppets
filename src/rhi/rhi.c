/*
 * Master of Puppets — RHI Dispatch
 * rhi.c — Backend resolution and name lookup
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/rhi.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Backend name table
 * ------------------------------------------------------------------------- */

const char *mop_backend_name(MopBackendType type) {
    switch (type) {
        case MOP_BACKEND_AUTO:   return "auto";
        case MOP_BACKEND_CPU:    return "cpu";
        case MOP_BACKEND_OPENGL: return "opengl";
        case MOP_BACKEND_VULKAN: return "vulkan";
        default:                 return "unknown";
    }
}

/* -------------------------------------------------------------------------
 * Platform default backend
 *
 * Preference order: OpenGL > CPU
 * Vulkan is not preferred as default because headless Vulkan availability
 * varies.  Applications that want Vulkan should request it explicitly.
 * ------------------------------------------------------------------------- */

MopBackendType mop_backend_default(void) {
#if defined(MOP_HAS_OPENGL)
    return MOP_BACKEND_OPENGL;
#else
    return MOP_BACKEND_CPU;
#endif
}

/* -------------------------------------------------------------------------
 * Backend resolution
 * ------------------------------------------------------------------------- */

const MopRhiBackend *mop_rhi_get_backend(MopBackendType type) {
    if (type == MOP_BACKEND_AUTO) {
        type = mop_backend_default();
    }

    switch (type) {
        case MOP_BACKEND_CPU:
            return mop_rhi_backend_cpu();

#if defined(MOP_HAS_OPENGL)
        case MOP_BACKEND_OPENGL:
            return mop_rhi_backend_opengl();
#endif

#if defined(MOP_HAS_VULKAN)
        case MOP_BACKEND_VULKAN:
            return mop_rhi_backend_vulkan();
#endif

        default:
            return NULL;
    }
}

/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * image_export.h — PNG image export
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_EXPORT_IMAGE_EXPORT_H
#define MOP_EXPORT_IMAGE_EXPORT_H

#include <mop/types.h>
#include <stdint.h>

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* Write viewport framebuffer to PNG.
 * Returns 0 on success, -1 on failure. */
int mop_export_png(MopViewport *vp, const char *path);

/* Write raw RGBA buffer to PNG.
 * Returns 0 on success, -1 on failure. */
int mop_export_png_buffer(const uint8_t *rgba, int width, int height,
                          const char *path);

#endif /* MOP_EXPORT_IMAGE_EXPORT_H */

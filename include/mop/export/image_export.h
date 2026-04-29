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

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* All export functions in this header use POSIX-style return codes:
 *   0  = success
 *  -1  = failure (file I/O, encoding, or invalid arguments).
 *
 * Note this is the inverse of bool — a return of 0 means it worked. */

/* Write viewport framebuffer to PNG.
 * Returns 0 on success, -1 on failure. */
int mop_export_png(MopViewport *vp, const char *path);

/* Write raw RGBA buffer to PNG.
 * Returns 0 on success, -1 on failure. */
int mop_export_png_buffer(const uint8_t *rgba, int width, int height,
                          const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MOP_EXPORT_IMAGE_EXPORT_H */

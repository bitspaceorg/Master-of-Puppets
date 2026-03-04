/*
 * Master of Puppets — PNG Image Export
 * image_export.c — Write viewport framebuffer or raw RGBA buffer to PNG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/core/viewport.h>
#include <mop/export/image_export.h>
#include <mop/util/log.h>

#include "stb_image_write.h"

int mop_export_png(MopViewport *vp, const char *path) {
  if (!vp || !path) {
    MOP_ERROR("mop_export_png: NULL viewport or path");
    return -1;
  }

  int w, h;
  const uint8_t *rgba = mop_viewport_read_color(vp, &w, &h);
  if (!rgba) {
    MOP_ERROR("mop_export_png: framebuffer readback failed");
    return -1;
  }

  return mop_export_png_buffer(rgba, w, h, path);
}

int mop_export_png_buffer(const uint8_t *rgba, int width, int height,
                          const char *path) {
  if (!rgba || !path || width <= 0 || height <= 0) {
    MOP_ERROR("mop_export_png_buffer: invalid arguments");
    return -1;
  }

  int stride = width * 4;
  if (!stbi_write_png(path, width, height, 4, rgba, stride)) {
    MOP_ERROR("mop_export_png_buffer: failed to write '%s'", path);
    return -1;
  }

  MOP_INFO("exported PNG %dx%d -> %s", width, height, path);
  return 0;
}

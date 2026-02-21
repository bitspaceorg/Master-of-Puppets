/*
 * Master of Puppets — Examples: Shared Geometry
 *
 * Header-only mesh data and utilities shared across example programs.
 * Provides cube, floor plane, UV sphere, and PPM writer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_EXAMPLES_GEOMETRY_H
#define MOP_EXAMPLES_GEOMETRY_H

#include <math.h>
#include <mop/mop.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Cube — 24 vertices (4 per face, distinct normals/colors), 36 indices
 * ------------------------------------------------------------------------- */

static const MopVertex CUBE_VERTICES[] = {
    /* Front (Z+) — red */
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {0, 0, 1},
     .color = {0.9f, 0.2f, 0.2f, 1}},
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {0, 0, 1},
     .color = {0.9f, 0.2f, 0.2f, 1}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {0, 0, 1},
     .color = {0.9f, 0.2f, 0.2f, 1}},
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {0, 0, 1},
     .color = {0.9f, 0.2f, 0.2f, 1}},
    /* Back (Z-) — green */
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {0, 0, -1},
     .color = {0.2f, 0.9f, 0.2f, 1}},
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {0, 0, -1},
     .color = {0.2f, 0.9f, 0.2f, 1}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {0, 0, -1},
     .color = {0.2f, 0.9f, 0.2f, 1}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {0, 0, -1},
     .color = {0.2f, 0.9f, 0.2f, 1}},
    /* Top (Y+) — blue */
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {0, 1, 0},
     .color = {0.2f, 0.2f, 0.9f, 1}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {0, 1, 0},
     .color = {0.2f, 0.2f, 0.9f, 1}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {0, 1, 0},
     .color = {0.2f, 0.2f, 0.9f, 1}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {0, 1, 0},
     .color = {0.2f, 0.2f, 0.9f, 1}},
    /* Bottom (Y-) — yellow */
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {0, -1, 0},
     .color = {0.9f, 0.9f, 0.2f, 1}},
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {0, -1, 0},
     .color = {0.9f, 0.9f, 0.2f, 1}},
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {0, -1, 0},
     .color = {0.9f, 0.9f, 0.2f, 1}},
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {0, -1, 0},
     .color = {0.9f, 0.9f, 0.2f, 1}},
    /* Right (X+) — cyan */
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {1, 0, 0},
     .color = {0.2f, 0.9f, 0.9f, 1}},
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {1, 0, 0},
     .color = {0.2f, 0.9f, 0.9f, 1}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {1, 0, 0},
     .color = {0.2f, 0.9f, 0.9f, 1}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {1, 0, 0},
     .color = {0.2f, 0.9f, 0.9f, 1}},
    /* Left (X-) — magenta */
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {-1, 0, 0},
     .color = {0.9f, 0.2f, 0.9f, 1}},
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {-1, 0, 0},
     .color = {0.9f, 0.2f, 0.9f, 1}},
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {-1, 0, 0},
     .color = {0.9f, 0.2f, 0.9f, 1}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {-1, 0, 0},
     .color = {0.9f, 0.2f, 0.9f, 1}},
};

static const uint32_t CUBE_INDICES[] = {
    0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
};

#define CUBE_VERTEX_COUNT 24
#define CUBE_INDEX_COUNT 36

/* -------------------------------------------------------------------------
 * Floor Plane — 4 vertices at y=0, extent [-5,5] in XZ
 * ------------------------------------------------------------------------- */

static const MopVertex PLANE_VERTICES[] = {
    {.position = {-5, 0, -5},
     .normal = {0, 1, 0},
     .color = {0.5f, 0.5f, 0.5f, 1}},
    {.position = {5, 0, -5},
     .normal = {0, 1, 0},
     .color = {0.5f, 0.5f, 0.5f, 1}},
    {.position = {5, 0, 5},
     .normal = {0, 1, 0},
     .color = {0.5f, 0.5f, 0.5f, 1}},
    {.position = {-5, 0, 5},
     .normal = {0, 1, 0},
     .color = {0.5f, 0.5f, 0.5f, 1}},
};

static const uint32_t PLANE_INDICES[] = {0, 1, 2, 2, 3, 0};

#define PLANE_VERTEX_COUNT 4
#define PLANE_INDEX_COUNT 6

/* -------------------------------------------------------------------------
 * UV Sphere — runtime generation
 *
 * Generates a lat/lon sphere with the given radius and color.
 * Writes into caller-provided arrays.  Returns the vertex count;
 * index count = lat * lon * 6.
 * ------------------------------------------------------------------------- */

#define SPHERE_MAX_VERTS ((32 + 1) * (48 + 1))
#define SPHERE_MAX_INDICES (32 * 48 * 6)

static inline uint32_t geometry_make_sphere(int lat, int lon, float radius,
                                            MopVertex *out_verts,
                                            uint32_t *out_indices,
                                            MopColor color) {
  uint32_t vi = 0;
  for (int i = 0; i <= lat; i++) {
    float theta = (float)i / (float)lat * 3.14159265f;
    float st = sinf(theta), ct = cosf(theta);
    for (int j = 0; j <= lon; j++) {
      float phi = (float)j / (float)lon * 6.28318530f;
      float sp = sinf(phi), cp = cosf(phi);
      MopVec3 n = {st * cp, ct, st * sp};
      out_verts[vi].position =
          (MopVec3){n.x * radius, n.y * radius, n.z * radius};
      out_verts[vi].normal = n;
      out_verts[vi].color = color;
      out_verts[vi].u = (float)j / (float)lon;
      out_verts[vi].v = (float)i / (float)lat;
      vi++;
    }
  }

  uint32_t ii = 0;
  for (int i = 0; i < lat; i++) {
    for (int j = 0; j < lon; j++) {
      uint32_t a = (uint32_t)(i * (lon + 1) + j);
      uint32_t b = a + (uint32_t)(lon + 1);
      out_indices[ii++] = a;
      out_indices[ii++] = b;
      out_indices[ii++] = a + 1;
      out_indices[ii++] = a + 1;
      out_indices[ii++] = b;
      out_indices[ii++] = b + 1;
    }
  }

  return vi;
}

/* -------------------------------------------------------------------------
 * PPM writer — saves RGBA8 framebuffer as Netpbm P6
 * ------------------------------------------------------------------------- */

static inline bool write_ppm(const char *path, const uint8_t *pixels, int width,
                             int height) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; i++)
    fwrite(&pixels[i * 4], 1, 3, f);
  fclose(f);
  return true;
}

#endif /* MOP_EXAMPLES_GEOMETRY_H */

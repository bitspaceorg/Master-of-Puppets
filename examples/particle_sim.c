/*
 * Master of Puppets — Interactive Particle Simulation (SDL3)
 *
 * Architecture:
 *   The APPLICATION owns all simulation logic:
 *     - Particle pool, spawn/kill lifecycle
 *     - Physics integration (velocity, gravity, lifetime)
 *     - 3D voxel density field (32x32x32)
 *     - Four display modes: billboard, volume slices, isosurface, point cloud
 *     - Billboard quad generation with proper camera-facing orientation
 *
 *   MOP owns only:
 *     - Rendering submitted geometry
 *     - Camera / input handling (orbit, pan, zoom)
 *     - Selection / gizmo system
 *     - Post-processing
 *
 *   Controls are via a left sidebar (ui_toolbar.h) — no keyboard shortcuts
 *   for simulation.  Emitters are repositioned via MOP's gizmo system:
 *   click the yellow octahedron marker, drag the translate gizmo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_toolbar.h"
#include <SDL3/SDL.h>
#include <mop/mop.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * APP-OWNED SIMULATION
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * PRNG (xorshift32)
 * ------------------------------------------------------------------------- */

static uint32_t xorshift32(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static float randf(uint32_t *rng) {
  return (float)(xorshift32(rng) & 0xFFFFFF) / (float)0xFFFFFF;
}

static float randf_range(uint32_t *rng, float lo, float hi) {
  return lo + randf(rng) * (hi - lo);
}

/* -------------------------------------------------------------------------
 * Particle
 * ------------------------------------------------------------------------- */

typedef struct {
  float x, y, z;
  float vx, vy, vz;
  float lifetime, max_lifetime;
  float size;
  float r, g, b, a;
  float temperature; /* 0..1 for fire color ramp */
  bool alive;
} Particle;

/* -------------------------------------------------------------------------
 * Emitter descriptor
 * ------------------------------------------------------------------------- */

typedef struct {
  Particle *pool;
  uint32_t max_particles;
  uint32_t alive_count;

  float emit_rate;
  float emit_accum;
  float lifetime_min, lifetime_max;
  float vx_min, vx_max, vy_min, vy_max, vz_min, vz_max;
  float gx, gy, gz;
  float size_start, size_end;
  float cr0, cg0, cb0, ca0;
  float cr1, cg1, cb1, ca1;

  float px, py, pz;
  bool active;
  uint32_t rng;

  MopBlendMode blend;
  const char *name;
} SimEmitter;

static void sim_emitter_alloc(SimEmitter *e, uint32_t max_particles) {
  e->pool = calloc(max_particles, sizeof(Particle));
  e->max_particles = max_particles;
  e->rng = 0xDEADBEEF + (uint32_t)(uintptr_t)e;
}

static void sim_emitter_free(SimEmitter *e) { free(e->pool); }

/* Run one simulation step (physics only — no mesh generation). */
static void sim_emitter_update(SimEmitter *e, float dt) {
  e->alive_count = 0;
  for (uint32_t i = 0; i < e->max_particles; i++) {
    Particle *p = &e->pool[i];
    if (!p->alive)
      continue;

    p->lifetime += dt;
    if (p->lifetime >= p->max_lifetime) {
      p->alive = false;
      continue;
    }

    p->vx += e->gx * dt;
    p->vy += e->gy * dt;
    p->vz += e->gz * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;

    float t = p->lifetime / p->max_lifetime;
    p->size = e->size_start + (e->size_end - e->size_start) * t;
    p->r = e->cr0 + (e->cr1 - e->cr0) * t;
    p->g = e->cg0 + (e->cg1 - e->cg0) * t;
    p->b = e->cb0 + (e->cb1 - e->cb0) * t;
    p->a = e->ca0 + (e->ca1 - e->ca0) * t;
    p->temperature = 1.0f - t;

    e->alive_count++;
  }

  if (e->active) {
    e->emit_accum += e->emit_rate * dt;
    while (e->emit_accum >= 1.0f) {
      e->emit_accum -= 1.0f;
      for (uint32_t i = 0; i < e->max_particles; i++) {
        Particle *p = &e->pool[i];
        if (p->alive)
          continue;

        p->alive = true;
        p->x = e->px;
        p->y = e->py;
        p->z = e->pz;
        p->vx = randf_range(&e->rng, e->vx_min, e->vx_max);
        p->vy = randf_range(&e->rng, e->vy_min, e->vy_max);
        p->vz = randf_range(&e->rng, e->vz_min, e->vz_max);
        p->lifetime = 0;
        p->max_lifetime =
            randf_range(&e->rng, e->lifetime_min, e->lifetime_max);
        p->size = e->size_start;
        p->r = e->cr0;
        p->g = e->cg0;
        p->b = e->cb0;
        p->a = e->ca0;
        p->temperature = 1.0f;
        e->alive_count++;
        break;
      }
    }
  }
}

/* -------------------------------------------------------------------------
 * Preset configurations
 * ------------------------------------------------------------------------- */

static void sim_preset_fire(SimEmitter *e) {
  sim_emitter_alloc(e, 512);
  e->emit_rate = 60;
  e->lifetime_min = 0.5f;
  e->lifetime_max = 1.5f;
  e->vx_min = -0.3f;
  e->vx_max = 0.3f;
  e->vy_min = 1.5f;
  e->vy_max = 3.0f;
  e->vz_min = -0.3f;
  e->vz_max = 0.3f;
  e->gx = 0;
  e->gy = 1.0f;
  e->gz = 0;
  e->size_start = 0.5f;
  e->size_end = 0.1f;
  e->cr0 = 1.0f;
  e->cg0 = 0.8f;
  e->cb0 = 0.2f;
  e->ca0 = 1.0f;
  e->cr1 = 0.8f;
  e->cg1 = 0.1f;
  e->cb1 = 0.0f;
  e->ca1 = 0.0f;
  e->blend = MOP_BLEND_ADDITIVE;
  e->active = true;
  e->name = "Fire";
}

static void sim_preset_smoke(SimEmitter *e) {
  sim_emitter_alloc(e, 256);
  e->emit_rate = 30;
  e->lifetime_min = 2.0f;
  e->lifetime_max = 4.0f;
  e->vx_min = -0.2f;
  e->vx_max = 0.2f;
  e->vy_min = 0.3f;
  e->vy_max = 0.8f;
  e->vz_min = -0.2f;
  e->vz_max = 0.2f;
  e->gx = 0;
  e->gy = 0.3f;
  e->gz = 0;
  e->size_start = 0.3f;
  e->size_end = 1.2f;
  e->cr0 = 0.5f;
  e->cg0 = 0.5f;
  e->cb0 = 0.5f;
  e->ca0 = 0.6f;
  e->cr1 = 0.3f;
  e->cg1 = 0.3f;
  e->cb1 = 0.3f;
  e->ca1 = 0.0f;
  e->blend = MOP_BLEND_ALPHA;
  e->active = true;
  e->name = "Smoke";
}

static void sim_preset_sparks(SimEmitter *e) {
  sim_emitter_alloc(e, 1024);
  e->emit_rate = 100;
  e->lifetime_min = 0.3f;
  e->lifetime_max = 0.8f;
  e->vx_min = -2.0f;
  e->vx_max = 2.0f;
  e->vy_min = 1.0f;
  e->vy_max = 4.0f;
  e->vz_min = -2.0f;
  e->vz_max = 2.0f;
  e->gx = 0;
  e->gy = -2.0f;
  e->gz = 0;
  e->size_start = 0.05f;
  e->size_end = 0.02f;
  e->cr0 = 1.0f;
  e->cg0 = 0.6f;
  e->cb0 = 0.1f;
  e->ca0 = 1.0f;
  e->cr1 = 1.0f;
  e->cg1 = 0.3f;
  e->cb1 = 0.0f;
  e->ca1 = 0.0f;
  e->blend = MOP_BLEND_ADDITIVE;
  e->active = true;
  e->name = "Sparks";
}

/* -------------------------------------------------------------------------
 * 3D Voxel Density Field
 * ------------------------------------------------------------------------- */

#define VOXEL_RES 32
#define VOXEL_EXTENT 4.0f /* half-extent in world units */

typedef struct {
  float density[VOXEL_RES * VOXEL_RES * VOXEL_RES];
  float temperature[VOXEL_RES * VOXEL_RES * VOXEL_RES];
} VoxelGrid;

static inline int voxel_idx(int x, int y, int z) {
  return z * VOXEL_RES * VOXEL_RES + y * VOXEL_RES + x;
}

static inline void voxel_world_to_grid(float wx, float wy, float wz, int *gx,
                                       int *gy, int *gz) {
  *gx = (int)((wx + VOXEL_EXTENT) / (2.0f * VOXEL_EXTENT) * (float)VOXEL_RES);
  *gy = (int)((wy) / (2.0f * VOXEL_EXTENT) * (float)VOXEL_RES);
  *gz = (int)((wz + VOXEL_EXTENT) / (2.0f * VOXEL_EXTENT) * (float)VOXEL_RES);
}

static inline void voxel_grid_to_world(int gx, int gy, int gz, float *wx,
                                       float *wy, float *wz) {
  *wx = -VOXEL_EXTENT +
        ((float)gx + 0.5f) / (float)VOXEL_RES * 2.0f * VOXEL_EXTENT;
  *wy = ((float)gy + 0.5f) / (float)VOXEL_RES * 2.0f * VOXEL_EXTENT;
  *wz = -VOXEL_EXTENT +
        ((float)gz + 0.5f) / (float)VOXEL_RES * 2.0f * VOXEL_EXTENT;
}

/* Clear and scatter all particles into the grid. */
static void voxel_grid_scatter(VoxelGrid *grid, SimEmitter emitters[3]) {
  memset(grid->density, 0, sizeof(grid->density));
  memset(grid->temperature, 0, sizeof(grid->temperature));

  for (int ei = 0; ei < 3; ei++) {
    SimEmitter *e = &emitters[ei];
    for (uint32_t i = 0; i < e->max_particles; i++) {
      Particle *p = &e->pool[i];
      if (!p->alive)
        continue;

      int cx, cy, cz;
      voxel_world_to_grid(p->x, p->y, p->z, &cx, &cy, &cz);

      /* 3x3x3 splat kernel */
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            int gx = cx + dx, gy = cy + dy, gz = cz + dz;
            if (gx < 0 || gx >= VOXEL_RES || gy < 0 || gy >= VOXEL_RES ||
                gz < 0 || gz >= VOXEL_RES)
              continue;

            /* Weight: center=1, face=0.5, edge=0.25, corner=0.125 */
            int dist = abs(dx) + abs(dy) + abs(dz);
            float w = 1.0f;
            if (dist == 1)
              w = 0.5f;
            else if (dist == 2)
              w = 0.25f;
            else if (dist == 3)
              w = 0.125f;

            int idx = voxel_idx(gx, gy, gz);
            grid->density[idx] += w * p->size;
            grid->temperature[idx] += w * p->temperature;
          }
        }
      }
    }
  }

  /* Normalize temperature by density */
  int total = VOXEL_RES * VOXEL_RES * VOXEL_RES;
  for (int i = 0; i < total; i++) {
    if (grid->density[i] > 0.001f) {
      grid->temperature[i] /= grid->density[i];
      if (grid->temperature[i] > 1.0f)
        grid->temperature[i] = 1.0f;
    }
  }
}

/* -------------------------------------------------------------------------
 * Fire color ramp: temperature 0->1 maps black->red->orange->yellow->white
 * ------------------------------------------------------------------------- */

static inline MopColor fire_color_ramp(float t) {
  MopColor c = {0, 0, 0, 1};
  if (t < 0.25f) {
    float s = t / 0.25f;
    c.r = s * 0.6f;
  } else if (t < 0.5f) {
    float s = (t - 0.25f) / 0.25f;
    c.r = 0.6f + s * 0.4f;
    c.g = s * 0.4f;
  } else if (t < 0.75f) {
    float s = (t - 0.5f) / 0.25f;
    c.r = 1.0f;
    c.g = 0.4f + s * 0.4f;
    c.b = s * 0.1f;
  } else {
    float s = (t - 0.75f) / 0.25f;
    c.r = 1.0f;
    c.g = 0.8f + s * 0.2f;
    c.b = 0.1f + s * 0.9f;
  }
  return c;
}

/* -------------------------------------------------------------------------
 * Display mode converters: all produce MopVertex[] + uint32_t[]
 * ------------------------------------------------------------------------- */

/* Max output sizes */
#define MAX_BILLBOARD_VERTS (2048 * 4)
#define MAX_BILLBOARD_IDX (2048 * 6)
#define MAX_VOLUME_VERTS (64 * 4)
#define MAX_VOLUME_IDX (64 * 6)
#define MAX_ISO_VERTS 65536
#define MAX_ISO_IDX 65536
#define MAX_PTCLOUD_VERTS (VOXEL_RES * VOXEL_RES * VOXEL_RES * 4)
#define MAX_PTCLOUD_IDX (VOXEL_RES * VOXEL_RES * VOXEL_RES * 6)

typedef enum {
  DISPLAY_BILLBOARD = 0,
  DISPLAY_VOLUME = 1,
  DISPLAY_ISOSURFACE = 2,
  DISPLAY_POINTCLOUD = 3
} DisplayMode;

/* --- Billboard mode: camera-facing quads per particle --- */

static void gen_billboards(SimEmitter emitters[3], MopVec3 cam_right,
                           MopVec3 cam_up, MopVertex *verts, uint32_t *indices,
                           uint32_t *out_vc, uint32_t *out_ic) {
  uint32_t vi = 0, ii = 0;

  for (int ei = 0; ei < 3; ei++) {
    SimEmitter *e = &emitters[ei];
    for (uint32_t i = 0; i < e->max_particles; i++) {
      Particle *p = &e->pool[i];
      if (!p->alive)
        continue;
      if (vi + 4 > MAX_BILLBOARD_VERTS)
        goto done;

      float hs = p->size * 0.5f;
      MopColor c = {p->r, p->g, p->b, p->a};
      MopVec3 n = {0, 0, 1};

      float rx = cam_right.x * hs, ry = cam_right.y * hs, rz = cam_right.z * hs;
      float ux = cam_up.x * hs, uy = cam_up.y * hs, uz = cam_up.z * hs;

      verts[vi + 0] = (MopVertex){
          .position = {p->x - rx - ux, p->y - ry - uy, p->z - rz - uz},
          .normal = n,
          .color = c};
      verts[vi + 1] = (MopVertex){
          .position = {p->x + rx - ux, p->y + ry - uy, p->z + rz - uz},
          .normal = n,
          .color = c};
      verts[vi + 2] = (MopVertex){
          .position = {p->x + rx + ux, p->y + ry + uy, p->z + rz + uz},
          .normal = n,
          .color = c};
      verts[vi + 3] = (MopVertex){
          .position = {p->x - rx + ux, p->y - ry + uy, p->z - rz + uz},
          .normal = n,
          .color = c};

      indices[ii + 0] = vi;
      indices[ii + 1] = vi + 1;
      indices[ii + 2] = vi + 2;
      indices[ii + 3] = vi + 2;
      indices[ii + 4] = vi + 3;
      indices[ii + 5] = vi;
      vi += 4;
      ii += 6;
    }
  }
done:
  *out_vc = vi;
  *out_ic = ii;
}

/* --- Volume slices: 64 camera-perpendicular quads back-to-front --- */

#define NUM_SLICES 64

static void gen_volume_slices(const VoxelGrid *grid, MopVec3 cam_eye,
                              MopVec3 cam_target, MopVertex *verts,
                              uint32_t *indices, uint32_t *out_vc,
                              uint32_t *out_ic) {
  MopVec3 fwd = mop_vec3_normalize(mop_vec3_sub(cam_target, cam_eye));
  MopVec3 world_up = {0, 1, 0};
  MopVec3 right = mop_vec3_normalize(mop_vec3_cross(fwd, world_up));
  MopVec3 up = mop_vec3_cross(right, fwd);

  uint32_t vi = 0, ii = 0;

  /* Slices from back to front along the view direction */
  for (int s = NUM_SLICES - 1; s >= 0; s--) {
    float t = (float)s / (float)(NUM_SLICES - 1);
    /* Slice center sweeps through the voxel volume */
    float slice_z = -VOXEL_EXTENT + t * 2.0f * VOXEL_EXTENT;
    MopVec3 center = mop_vec3_add(mop_vec3_scale(fwd, slice_z),
                                  (MopVec3){0, VOXEL_EXTENT, 0});

    /* Sample density at this slice */
    float avg_density = 0;
    float avg_temp = 0;
    int samples = 0;
    for (int gy = 0; gy < VOXEL_RES; gy += 4) {
      for (int gx = 0; gx < VOXEL_RES; gx += 4) {
        int gz = (int)(t * (float)(VOXEL_RES - 1));
        if (gz >= VOXEL_RES)
          gz = VOXEL_RES - 1;
        int idx = voxel_idx(gx, gy, gz);
        avg_density += grid->density[idx];
        avg_temp += grid->temperature[idx];
        samples++;
      }
    }
    if (samples > 0) {
      avg_density /= (float)samples;
      avg_temp /= (float)samples;
    }

    if (avg_density < 0.01f)
      continue;
    if (avg_density > 1.0f)
      avg_density = 1.0f;

    MopColor c = fire_color_ramp(avg_temp);
    c.a = avg_density * 0.4f;
    if (c.a > 0.8f)
      c.a = 0.8f;

    float ext = VOXEL_EXTENT;
    MopVec3 n = fwd;

    verts[vi + 0] =
        (MopVertex){.position = {center.x - right.x * ext - up.x * ext,
                                 center.y - right.y * ext - up.y * ext,
                                 center.z - right.z * ext - up.z * ext},
                    .normal = n,
                    .color = c};
    verts[vi + 1] =
        (MopVertex){.position = {center.x + right.x * ext - up.x * ext,
                                 center.y + right.y * ext - up.y * ext,
                                 center.z + right.z * ext - up.z * ext},
                    .normal = n,
                    .color = c};
    verts[vi + 2] =
        (MopVertex){.position = {center.x + right.x * ext + up.x * ext,
                                 center.y + right.y * ext + up.y * ext,
                                 center.z + right.z * ext + up.z * ext},
                    .normal = n,
                    .color = c};
    verts[vi + 3] =
        (MopVertex){.position = {center.x - right.x * ext + up.x * ext,
                                 center.y - right.y * ext + up.y * ext,
                                 center.z - right.z * ext + up.z * ext},
                    .normal = n,
                    .color = c};

    indices[ii + 0] = vi;
    indices[ii + 1] = vi + 1;
    indices[ii + 2] = vi + 2;
    indices[ii + 3] = vi + 2;
    indices[ii + 4] = vi + 3;
    indices[ii + 5] = vi;
    vi += 4;
    ii += 6;
  }

  *out_vc = vi;
  *out_ic = ii;
}

/* --- Isosurface: Marching cubes at density threshold --- */

/* Marching cubes edge table and triangle table (standard 256-entry) */
/* Abbreviated — full tables below */

static const int MC_EDGE_TABLE[256] = {
    0x0,   0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c, 0x80c, 0x905, 0xa0f,
    0xb06, 0xc0a, 0xd03, 0xe09, 0xf00, 0x190, 0x99,  0x393, 0x29a, 0x596, 0x49f,
    0x795, 0x69c, 0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90, 0x230,
    0x339, 0x33,  0x13a, 0x636, 0x73f, 0x435, 0x53c, 0xa3c, 0xb35, 0x83f, 0x936,
    0xe3a, 0xf33, 0xc39, 0xd30, 0x3a0, 0x2a9, 0x1a3, 0xaa,  0x7a6, 0x6af, 0x5a5,
    0x4ac, 0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0, 0x460, 0x569,
    0x663, 0x76a, 0x66,  0x16f, 0x265, 0x36c, 0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a,
    0x963, 0xa69, 0xb60, 0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff,  0x3f5, 0x2fc,
    0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0, 0x650, 0x759, 0x453,
    0x55a, 0x256, 0x35f, 0x55,  0x15c, 0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53,
    0x859, 0x950, 0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc,  0xfcc,
    0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0, 0x8c0, 0x9c9, 0xac3, 0xbca,
    0xcc6, 0xdcf, 0xec5, 0xfcc, 0xcc,  0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9,
    0x7c0, 0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c, 0x15c, 0x55,
    0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650, 0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6,
    0xfff, 0xcf5, 0xdfc, 0x2fc, 0x3f5, 0xff,  0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
    0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c, 0x36c, 0x265, 0x16f,
    0x66,  0x76a, 0x663, 0x569, 0x460, 0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af,
    0xaa5, 0xbac, 0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa,  0x1a3, 0x2a9, 0x3a0, 0xd30,
    0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c, 0x53c, 0x435, 0x73f, 0x636,
    0x13a, 0x33,  0x339, 0x230, 0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895,
    0x99c, 0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99,  0x190, 0xf00, 0xe09,
    0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c, 0x70c, 0x605, 0x50f, 0x406, 0x30a,
    0x203, 0x109, 0x0};

static const int MC_TRI_TABLE[256][16] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 9, 8, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 0, 2, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 8, 3, 2, 10, 8, 10, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 8, 11, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 2, 1, 9, 11, 9, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 1, 11, 10, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 10, 1, 0, 8, 10, 8, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {3, 9, 0, 3, 11, 9, 11, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 7, 3, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 1, 9, 4, 7, 1, 7, 3, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 4, 7, 3, 0, 4, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 9, 0, 2, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {8, 4, 7, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 4, 7, 11, 2, 4, 2, 0, 4, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 8, 4, 7, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1, -1, -1, -1},
    {3, 10, 1, 3, 11, 10, 7, 8, 4, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1, -1, -1, -1},
    {4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {4, 7, 11, 4, 11, 9, 9, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 1, 5, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 5, 4, 8, 3, 5, 3, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 10, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 2, 10, 5, 4, 2, 4, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1, -1, -1, -1},
    {9, 5, 4, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 0, 8, 11, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 0, 1, 5, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1, -1, -1, -1},
    {10, 3, 11, 10, 1, 3, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1, -1, -1, -1},
    {5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {5, 4, 8, 5, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 5, 7, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 3, 0, 9, 5, 3, 5, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 8, 0, 1, 7, 1, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {1, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 9, 5, 7, 10, 1, 2, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1, -1, -1, -1},
    {8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1, -1, -1, -1},
    {2, 10, 5, 2, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {7, 9, 5, 7, 8, 9, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1, -1, -1, -1},
    {11, 2, 1, 11, 1, 7, 7, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1, -1, -1, -1},
    {5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
    {11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1},
    {11, 10, 5, 7, 11, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 1, 9, 8, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 2, 6, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 1, 2, 6, 3, 0, 8, -1, -1, -1, -1, -1, -1, -1},
    {9, 6, 5, 9, 0, 6, 0, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 0, 8, 11, 2, 0, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1, -1, -1, -1},
    {6, 3, 11, 6, 5, 3, 5, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1, -1, -1, -1},
    {6, 5, 9, 6, 9, 11, 11, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 4, 7, 3, 6, 5, 10, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 5, 10, 6, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {6, 1, 2, 6, 5, 1, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1, -1, -1, -1},
    {8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1, -1, -1, -1},
    {7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1},
    {3, 11, 2, 7, 8, 4, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1},
    {9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
    {8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
    {0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1},
    {6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1, -1, -1, -1},
    {10, 4, 9, 6, 4, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 10, 6, 4, 9, 10, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1},
    {10, 0, 1, 10, 6, 0, 6, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {1, 4, 9, 1, 2, 4, 2, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1, -1, -1, -1},
    {0, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 2, 8, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 4, 9, 10, 6, 4, 11, 2, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1, -1, -1, -1},
    {3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1},
    {9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1, -1, -1, -1},
    {8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1},
    {3, 11, 6, 3, 6, 0, 0, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {6, 4, 8, 11, 6, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 10, 6, 7, 8, 10, 8, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1, -1, -1, -1},
    {10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1, -1, -1, -1},
    {10, 6, 7, 10, 7, 1, 1, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
    {7, 8, 0, 7, 0, 6, 6, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {7, 3, 2, 6, 7, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1},
    {1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
    {11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1, -1, -1, -1},
    {8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
    {0, 9, 1, 11, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1, -1, -1, -1},
    {7, 11, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 1, 9, 8, 3, 1, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 8, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 9, 0, 2, 10, 9, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1, -1, -1, -1},
    {7, 2, 3, 6, 2, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 0, 8, 7, 6, 0, 6, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {2, 7, 6, 2, 3, 7, 0, 1, 9, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1, -1, -1, -1},
    {10, 7, 6, 10, 1, 7, 1, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1, -1, -1, -1},
    {0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1, -1, -1, -1},
    {7, 6, 10, 7, 10, 8, 8, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {6, 8, 4, 11, 8, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 6, 11, 3, 0, 6, 0, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 6, 11, 8, 4, 6, 9, 0, 1, -1, -1, -1, -1, -1, -1, -1},
    {9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1, -1, -1, -1},
    {6, 8, 4, 6, 11, 8, 2, 10, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1, -1, -1, -1},
    {4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1, -1, -1, -1},
    {10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
    {8, 2, 3, 8, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1, -1, -1, -1},
    {1, 9, 4, 1, 4, 2, 2, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 1, 3, 8, 6, 1, 8, 4, 6, 6, 10, 1, -1, -1, -1, -1},
    {10, 1, 0, 10, 0, 6, 6, 0, 4, -1, -1, -1, -1, -1, -1, -1},
    {4, 6, 3, 4, 3, 8, 6, 10, 3, 0, 3, 9, 10, 9, 3, -1},
    {10, 9, 4, 6, 10, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 5, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 1, 5, 4, 0, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1, -1, -1, -1},
    {9, 5, 4, 10, 1, 2, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1, -1, -1, -1},
    {7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1, -1, -1, -1},
    {3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1},
    {7, 2, 3, 7, 6, 2, 5, 4, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1, -1, -1, -1},
    {3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1, -1, -1, -1},
    {6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
    {9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1, -1, -1, -1},
    {1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
    {4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1},
    {7, 6, 10, 7, 10, 8, 5, 4, 10, 4, 8, 10, -1, -1, -1, -1},
    {6, 9, 5, 6, 11, 9, 11, 8, 9, -1, -1, -1, -1, -1, -1, -1},
    {3, 6, 11, 0, 6, 3, 0, 5, 6, 0, 9, 5, -1, -1, -1, -1},
    {0, 11, 8, 0, 5, 11, 0, 1, 5, 5, 6, 11, -1, -1, -1, -1},
    {6, 11, 3, 6, 3, 5, 5, 3, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 9, 5, 11, 9, 11, 8, 11, 5, 6, -1, -1, -1, -1},
    {0, 11, 3, 0, 6, 11, 0, 9, 6, 5, 6, 9, 1, 2, 10, -1},
    {11, 8, 5, 11, 5, 6, 8, 0, 5, 10, 5, 2, 0, 2, 5, -1},
    {6, 11, 3, 6, 3, 5, 2, 10, 3, 10, 5, 3, -1, -1, -1, -1},
    {5, 8, 9, 5, 2, 8, 5, 6, 2, 3, 8, 2, -1, -1, -1, -1},
    {9, 5, 6, 9, 6, 0, 0, 6, 2, -1, -1, -1, -1, -1, -1, -1},
    {1, 5, 8, 1, 8, 0, 5, 6, 8, 3, 8, 2, 6, 2, 8, -1},
    {1, 5, 6, 2, 1, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1},
    {10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1, -1, -1, -1},
    {0, 3, 8, 5, 6, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {10, 5, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 7, 5, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 11, 7, 5, 8, 3, 0, -1, -1, -1, -1, -1, -1, -1},
    {5, 11, 7, 5, 10, 11, 1, 9, 0, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1, -1, -1, -1},
    {11, 1, 2, 11, 7, 1, 7, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1, -1, -1, -1},
    {9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1, -1, -1, -1},
    {7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1},
    {2, 5, 10, 2, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1, -1, -1, -1},
    {9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
    {1, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 7, 0, 7, 1, 1, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 3, 9, 3, 5, 5, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 7, 5, 9, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {5, 8, 4, 5, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1, -1, -1, -1},
    {10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1},
    {2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1, -1, -1, -1},
    {0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1},
    {0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
    {9, 4, 5, 2, 11, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1, -1, -1, -1},
    {5, 10, 2, 5, 2, 4, 4, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1},
    {5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 3, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 5, 1, 0, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1, -1, -1, -1},
    {9, 4, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 11, 7, 4, 9, 11, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1, -1, -1, -1},
    {1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1, -1, -1, -1},
    {3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
    {4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1, -1, -1, -1},
    {9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
    {11, 7, 4, 11, 4, 2, 2, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1, -1, -1, -1},
    {2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1, -1, -1, -1},
    {9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1},
    {3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
    {1, 10, 2, 8, 7, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 7, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1, -1, -1, -1},
    {4, 0, 3, 7, 4, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 8, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 11, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 10, 0, 10, 8, 8, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 1, 10, 11, 3, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 11, 1, 11, 9, 9, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1, -1, -1, -1},
    {0, 2, 11, 8, 0, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 2, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 10, 8, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 2, 0, 9, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1, -1, -1, -1},
    {1, 10, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 8, 9, 1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 9, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 3, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};

static inline MopVec3 mc_interp(MopVec3 p1, MopVec3 p2, float v1, float v2,
                                float iso) {
  if (fabsf(v1 - v2) < 1e-6f)
    return p1;
  float t = (iso - v1) / (v2 - v1);
  return (MopVec3){p1.x + t * (p2.x - p1.x), p1.y + t * (p2.y - p1.y),
                   p1.z + t * (p2.z - p1.z)};
}

static void gen_isosurface(const VoxelGrid *grid, float iso_threshold,
                           MopVertex *verts, uint32_t *indices,
                           uint32_t *out_vc, uint32_t *out_ic) {
  uint32_t vi = 0, ii = 0;
  float cell = 2.0f * VOXEL_EXTENT / (float)VOXEL_RES;

  for (int z = 0; z < VOXEL_RES - 1; z++) {
    for (int y = 0; y < VOXEL_RES - 1; y++) {
      for (int x = 0; x < VOXEL_RES - 1; x++) {
        /* 8 corner values */
        float val[8];
        float temp[8];
        MopVec3 pos[8];
        int corners[8][3] = {{x, y, z},
                             {x + 1, y, z},
                             {x + 1, y + 1, z},
                             {x, y + 1, z},
                             {x, y, z + 1},
                             {x + 1, y, z + 1},
                             {x + 1, y + 1, z + 1},
                             {x, y + 1, z + 1}};

        for (int c = 0; c < 8; c++) {
          int idx = voxel_idx(corners[c][0], corners[c][1], corners[c][2]);
          val[c] = grid->density[idx];
          temp[c] = grid->temperature[idx];
          voxel_grid_to_world(corners[c][0], corners[c][1], corners[c][2],
                              &pos[c].x, &pos[c].y, &pos[c].z);
        }

        /* Cube index */
        int cube_idx = 0;
        for (int c = 0; c < 8; c++)
          if (val[c] >= iso_threshold)
            cube_idx |= (1 << c);

        if (MC_EDGE_TABLE[cube_idx] == 0)
          continue;

        /* Edge vertices */
        MopVec3 edge_verts[12];
        float edge_temps[12];
        int edge_pairs[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                 {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                 {0, 4}, {1, 5}, {2, 6}, {3, 7}};

        for (int e = 0; e < 12; e++) {
          if (MC_EDGE_TABLE[cube_idx] & (1 << e)) {
            int a = edge_pairs[e][0], b = edge_pairs[e][1];
            edge_verts[e] =
                mc_interp(pos[a], pos[b], val[a], val[b], iso_threshold);
            float t_interp = (fabsf(val[a] - val[b]) < 1e-6f)
                                 ? 0.5f
                                 : (iso_threshold - val[a]) / (val[b] - val[a]);
            edge_temps[e] = temp[a] + t_interp * (temp[b] - temp[a]);
          }
        }

        /* Generate triangles */
        for (int t = 0; MC_TRI_TABLE[cube_idx][t] != -1; t += 3) {
          if (vi + 3 > MAX_ISO_VERTS || ii + 3 > MAX_ISO_IDX)
            goto iso_done;

          int e0 = MC_TRI_TABLE[cube_idx][t];
          int e1 = MC_TRI_TABLE[cube_idx][t + 1];
          int e2 = MC_TRI_TABLE[cube_idx][t + 2];

          MopVec3 p0 = edge_verts[e0];
          MopVec3 p1 = edge_verts[e1];
          MopVec3 p2 = edge_verts[e2];

          /* Compute face normal */
          MopVec3 ab = mop_vec3_sub(p1, p0);
          MopVec3 ac = mop_vec3_sub(p2, p0);
          MopVec3 n = mop_vec3_normalize(mop_vec3_cross(ab, ac));

          float t0 = edge_temps[e0];
          float t1 = edge_temps[e1];
          float t2 = edge_temps[e2];

          verts[vi + 0] = (MopVertex){
              .position = p0, .normal = n, .color = fire_color_ramp(t0)};
          verts[vi + 1] = (MopVertex){
              .position = p1, .normal = n, .color = fire_color_ramp(t1)};
          verts[vi + 2] = (MopVertex){
              .position = p2, .normal = n, .color = fire_color_ramp(t2)};

          indices[ii + 0] = vi;
          indices[ii + 1] = vi + 1;
          indices[ii + 2] = vi + 2;
          vi += 3;
          ii += 3;
        }
      }
    }
  }
iso_done:
  *out_vc = vi;
  *out_ic = ii;
  (void)cell;
}

/* --- Point cloud: tiny billboard quads at high-density voxel centers --- */

static void gen_pointcloud(const VoxelGrid *grid, float density_threshold,
                           MopVec3 cam_right, MopVec3 cam_up, MopVertex *verts,
                           uint32_t *indices, uint32_t *out_vc,
                           uint32_t *out_ic) {
  uint32_t vi = 0, ii = 0;
  float hs = 0.06f; /* tiny quad half-size */

  for (int z = 0; z < VOXEL_RES; z++) {
    for (int y = 0; y < VOXEL_RES; y++) {
      for (int x = 0; x < VOXEL_RES; x++) {
        int idx = voxel_idx(x, y, z);
        if (grid->density[idx] < density_threshold)
          continue;
        if (vi + 4 > MAX_PTCLOUD_VERTS)
          goto pc_done;

        float wx, wy, wz;
        voxel_grid_to_world(x, y, z, &wx, &wy, &wz);

        MopColor c = fire_color_ramp(grid->temperature[idx]);
        float alpha = grid->density[idx] / 5.0f;
        if (alpha > 1.0f)
          alpha = 1.0f;
        c.a = alpha;

        MopVec3 n = {0, 0, 1};
        float rx = cam_right.x * hs, ry = cam_right.y * hs,
              rz = cam_right.z * hs;
        float ux = cam_up.x * hs, uy = cam_up.y * hs, uz = cam_up.z * hs;

        verts[vi + 0] =
            (MopVertex){.position = {wx - rx - ux, wy - ry - uy, wz - rz - uz},
                        .normal = n,
                        .color = c};
        verts[vi + 1] =
            (MopVertex){.position = {wx + rx - ux, wy + ry - uy, wz + rz - uz},
                        .normal = n,
                        .color = c};
        verts[vi + 2] =
            (MopVertex){.position = {wx + rx + ux, wy + ry + uy, wz + rz + uz},
                        .normal = n,
                        .color = c};
        verts[vi + 3] =
            (MopVertex){.position = {wx - rx + ux, wy - ry + uy, wz - rz + uz},
                        .normal = n,
                        .color = c};

        indices[ii + 0] = vi;
        indices[ii + 1] = vi + 1;
        indices[ii + 2] = vi + 2;
        indices[ii + 3] = vi + 2;
        indices[ii + 4] = vi + 3;
        indices[ii + 5] = vi;
        vi += 4;
        ii += 6;
      }
    }
  }
pc_done:
  *out_vc = vi;
  *out_ic = ii;
}

/* -------------------------------------------------------------------------
 * Octahedron marker mesh (6 verts, 8 tris) — for emitter gizmo markers
 * ------------------------------------------------------------------------- */

#define MARKER_SCALE 0.15f

static void make_octahedron_marker(MopVertex *verts, uint32_t *indices,
                                   MopColor color) {
  float s = MARKER_SCALE;
  MopVec3 positions[6] = {
      {0, s, 0},  /* top */
      {0, -s, 0}, /* bottom */
      {s, 0, 0},  /* +x */
      {-s, 0, 0}, /* -x */
      {0, 0, s},  /* +z */
      {0, 0, -s}  /* -z */
  };

  /* 8 triangles */
  uint32_t faces[24] = {0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2,
                        1, 4, 2, 1, 3, 4, 1, 5, 3, 1, 2, 5};

  for (int i = 0; i < 6; i++) {
    verts[i] = (MopVertex){.position = positions[i],
                           .normal = mop_vec3_normalize(positions[i]),
                           .color = color};
  }
  memcpy(indices, faces, 24 * sizeof(uint32_t));
}

/* =========================================================================
 * MOP INTEGRATION
 * ========================================================================= */

/* Static ground plane */
static const MopVertex GROUND_VERTS[] = {
    {.position = {-6, 0, -6},
     .normal = {0, 1, 0},
     .color = {0.25f, 0.25f, 0.28f, 1}},
    {.position = {6, 0, -6},
     .normal = {0, 1, 0},
     .color = {0.25f, 0.25f, 0.28f, 1}},
    {.position = {6, 0, 6},
     .normal = {0, 1, 0},
     .color = {0.25f, 0.25f, 0.28f, 1}},
    {.position = {-6, 0, 6},
     .normal = {0, 1, 0},
     .color = {0.25f, 0.25f, 0.28f, 1}},
};
static const uint32_t GROUND_IDX[] = {0, 1, 2, 2, 3, 0};

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow("MOP — Particle Simulation", 960, 720,
                                        SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *sdl_renderer = SDL_CreateRenderer(window, NULL);
  if (!sdl_renderer) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(sdl_renderer, 1);

  /* ---- MOP: create viewport ---- */
  int win_w = 960, win_h = 720;
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = win_w, .height = win_h, .backend = MOP_BACKEND_VULKAN});
  if (!vp) {
    fprintf(stderr, "Failed to create viewport\n");
    return 1;
  }

  mop_viewport_set_clear_color(vp, (MopColor){0.06f, 0.06f, 0.09f, 1});
  mop_viewport_set_camera(vp, (MopVec3){4, 3, 6}, (MopVec3){0, 0.8f, 0},
                          (MopVec3){0, 1, 0}, 55.0f, 0.1f, 100.0f);
  mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_SHADING,
                                          .value = MOP_SHADING_SMOOTH});

  /* Static ground */
  mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = GROUND_VERTS,
                                           .vertex_count = 4,
                                           .indices = GROUND_IDX,
                                           .index_count = 6,
                                           .object_id = 1});

  uint32_t post_effects = MOP_POST_GAMMA;
  mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_POST_EFFECTS,
                                          .value = post_effects});
  mop_viewport_set_fog(vp, &(MopFogParams){.color = {0.06f, 0.06f, 0.09f, 1},
                                           .near_dist = 8.0f,
                                           .far_dist = 40.0f});

  /* ---- APP: create simulation emitters ---- */
  SimEmitter emitters[3];
  memset(emitters, 0, sizeof(emitters));
  sim_preset_fire(&emitters[0]);
  emitters[0].px = 0;
  emitters[0].py = 0.1f;
  emitters[0].pz = 0;
  sim_preset_smoke(&emitters[1]);
  emitters[1].px = 0;
  emitters[1].py = 0.9f;
  emitters[1].pz = 0;
  sim_preset_sparks(&emitters[2]);
  emitters[2].px = 0;
  emitters[2].py = 0.3f;
  emitters[2].pz = 0;

  /* ---- Emitter marker meshes (octahedrons, for gizmo picking) ---- */
  MopColor marker_color = {1.0f, 0.85f, 0.1f, 1.0f}; /* yellow */
  MopVertex marker_verts[6];
  uint32_t marker_indices[24];
  make_octahedron_marker(marker_verts, marker_indices, marker_color);

#define MARKER_BASE_ID 100
  MopMesh *marker_meshes[3];
  for (int i = 0; i < 3; i++) {
    marker_meshes[i] = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = marker_verts,
                           .vertex_count = 6,
                           .indices = marker_indices,
                           .index_count = 24,
                           .object_id = MARKER_BASE_ID + (uint32_t)i});
    mop_mesh_set_position(
        marker_meshes[i],
        (MopVec3){emitters[i].px, emitters[i].py, emitters[i].pz});
  }

  /* ---- Display mesh handle (rebuilt each frame) ---- */
  MopMesh *display_mesh = NULL;

  /* ---- Voxel grid ---- */
  VoxelGrid *voxel_grid = calloc(1, sizeof(VoxelGrid));

  /* ---- Display mode output buffers ---- */
  MopVertex *disp_verts = calloc(MAX_ISO_VERTS, sizeof(MopVertex));
  uint32_t *disp_idx = calloc(MAX_ISO_IDX, sizeof(uint32_t));

  /* ---- Sidebar ---- */
  UiToolbar toolbar;
  ui_toolbar_init(&toolbar);

  ui_toolbar_section(&toolbar, "EMITTERS");
  int btn_fire = ui_toolbar_button(&toolbar, "Fire", UI_BTN_TOGGLE, 0, true);
  int btn_smoke = ui_toolbar_button(&toolbar, "Smoke", UI_BTN_TOGGLE, 0, true);
  int btn_sparks =
      ui_toolbar_button(&toolbar, "Sparks", UI_BTN_TOGGLE, 0, true);

  ui_toolbar_section(&toolbar, "DISPLAY");
  int btn_billboard =
      ui_toolbar_button(&toolbar, "Billboard", UI_BTN_RADIO, 1, true);
  int btn_volume =
      ui_toolbar_button(&toolbar, "Volume", UI_BTN_RADIO, 1, false);
  int btn_iso =
      ui_toolbar_button(&toolbar, "Isosurface", UI_BTN_RADIO, 1, false);
  int btn_ptcloud =
      ui_toolbar_button(&toolbar, "Point Cloud", UI_BTN_RADIO, 1, false);

  ui_toolbar_section(&toolbar, "POST FX");
  int btn_gamma = ui_toolbar_button(&toolbar, "Gamma", UI_BTN_TOGGLE, 0, true);
  int btn_tonemap =
      ui_toolbar_button(&toolbar, "Tonemap", UI_BTN_TOGGLE, 0, false);
  int btn_vignette =
      ui_toolbar_button(&toolbar, "Vignette", UI_BTN_TOGGLE, 0, false);
  int btn_fog = ui_toolbar_button(&toolbar, "Fog", UI_BTN_TOGGLE, 0, false);

  ui_toolbar_section(&toolbar, "SIM");
  int btn_pause = ui_toolbar_button(&toolbar, "Pause", UI_BTN_TOGGLE, 0, false);
  int btn_reset =
      ui_toolbar_button(&toolbar, "Reset", UI_BTN_MOMENTARY, 0, false);

  ui_toolbar_section(&toolbar, "SHADING");
  int btn_smooth = ui_toolbar_button(&toolbar, "Smooth", UI_BTN_RADIO, 2, true);
  int btn_flat = ui_toolbar_button(&toolbar, "Flat", UI_BTN_RADIO, 2, false);
  int btn_wire =
      ui_toolbar_button(&toolbar, "Wireframe", UI_BTN_RADIO, 2, false);

  ui_toolbar_layout(&toolbar);

  /* ---- State ---- */
  bool paused = false;
  float sim_time = 0;
  DisplayMode display_mode = DISPLAY_BILLBOARD;

  SDL_Texture *tex =
      SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

  Uint64 last = SDL_GetPerformanceCounter();
  Uint64 freq_ = SDL_GetPerformanceFrequency();
  bool running = true;

  while (running) {
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last) / (float)freq_;
    last = now;
    if (dt > 0.1f)
      dt = 0.1f;

    /* ---- SDL events ---- */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT) {
        running = false;
        continue;
      }

      /* Keyboard shortcuts — toggle toolbar buttons so UI stays in sync */
      if (ev.type == SDL_EVENT_KEY_DOWN) {
        switch (ev.key.key) {
        case SDLK_ESCAPE:
          running = false;
          continue;

        /* Emitter toggles */
        case SDLK_1:
          ui_toolbar_toggle(&toolbar, btn_fire);
          break;
        case SDLK_2:
          ui_toolbar_toggle(&toolbar, btn_smoke);
          break;
        case SDLK_3:
          ui_toolbar_toggle(&toolbar, btn_sparks);
          break;

        /* Display mode cycle (4 = next) */
        case SDLK_4: {
          int modes[] = {btn_billboard, btn_volume, btn_iso, btn_ptcloud};
          int cur = 0;
          for (int i = 0; i < 4; i++)
            if (ui_toolbar_is_on(&toolbar, modes[i])) {
              cur = i;
              break;
            }
          ui_toolbar_radio_select(&toolbar, modes[(cur + 1) % 4]);
          break;
        }

        /* Post-processing toggles */
        case SDLK_G:
          ui_toolbar_toggle(&toolbar, btn_gamma);
          break;
        case SDLK_T:
          ui_toolbar_toggle(&toolbar, btn_tonemap);
          break;
        case SDLK_V:
          ui_toolbar_toggle(&toolbar, btn_vignette);
          break;
        case SDLK_F:
          ui_toolbar_toggle(&toolbar, btn_fog);
          break;

        /* Shading mode cycle */
        case SDLK_L: {
          int modes[] = {btn_smooth, btn_flat, btn_wire};
          int cur = 0;
          for (int i = 0; i < 3; i++)
            if (ui_toolbar_is_on(&toolbar, modes[i])) {
              cur = i;
              break;
            }
          ui_toolbar_radio_select(&toolbar, modes[(cur + 1) % 3]);
          break;
        }

        /* Sim controls */
        case SDLK_SPACE:
          ui_toolbar_toggle(&toolbar, btn_pause);
          break;
        case SDLK_R:
          ui_toolbar_toggle(&toolbar, btn_reset);
          break;

        default:
          break;
        }
        continue;
      }

      /* Sidebar consumes mouse events in its area */
      if (ui_toolbar_event(&toolbar, &ev))
        continue;

      /* Forward to MOP */
      switch (ev.type) {
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_DOWN,
                                                  ev.button.x, ev.button.y});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_SECONDARY_DOWN,
                                                  ev.button.x, ev.button.y});
        break;
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_UP,
                                                  ev.button.x, ev.button.y});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_SECONDARY_UP});
        break;
      case SDL_EVENT_MOUSE_MOTION:
        mop_viewport_input(
            vp, &(MopInputEvent){MOP_INPUT_POINTER_MOVE, ev.motion.x,
                                 ev.motion.y, ev.motion.xrel, ev.motion.yrel});
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SCROLL,
                                                .scroll = ev.wheel.y});
        break;
      case SDL_EVENT_WINDOW_RESIZED: {
        int w = ev.window.data1, h = ev.window.data2;
        if (w > 0 && h > 0) {
          win_w = w;
          win_h = h;
          mop_viewport_resize(vp, w, h);
          SDL_DestroyTexture(tex);
          tex = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        break;
      }
      default:
        break;
      }
    }

    /* ---- Poll MOP events (gizmo transforms) ---- */
    MopEvent mop_ev;
    while (mop_viewport_poll_event(vp, &mop_ev)) {
      if (mop_ev.type == MOP_EVENT_TRANSFORM_CHANGED) {
        for (int i = 0; i < 3; i++) {
          if (mop_ev.object_id == (uint32_t)(MARKER_BASE_ID + i)) {
            emitters[i].px = mop_ev.position.x;
            emitters[i].py = mop_ev.position.y;
            emitters[i].pz = mop_ev.position.z;
          }
        }
      }
    }

    /* ---- Sync sidebar state ---- */
    emitters[0].active = ui_toolbar_is_on(&toolbar, btn_fire);
    emitters[1].active = ui_toolbar_is_on(&toolbar, btn_smoke);
    emitters[2].active = ui_toolbar_is_on(&toolbar, btn_sparks);
    paused = ui_toolbar_is_on(&toolbar, btn_pause);

    if (ui_toolbar_fired(&toolbar, btn_reset)) {
      sim_time = 0;
      for (int i = 0; i < 3; i++) {
        memset(emitters[i].pool, 0,
               emitters[i].max_particles * sizeof(Particle));
        emitters[i].alive_count = 0;
        emitters[i].emit_accum = 0;
      }
    }

    /* Display mode */
    if (ui_toolbar_is_on(&toolbar, btn_billboard))
      display_mode = DISPLAY_BILLBOARD;
    if (ui_toolbar_is_on(&toolbar, btn_volume))
      display_mode = DISPLAY_VOLUME;
    if (ui_toolbar_is_on(&toolbar, btn_iso))
      display_mode = DISPLAY_ISOSURFACE;
    if (ui_toolbar_is_on(&toolbar, btn_ptcloud))
      display_mode = DISPLAY_POINTCLOUD;

    /* Post-processing — send via MOP event */
    post_effects = 0;
    if (ui_toolbar_is_on(&toolbar, btn_gamma))
      post_effects |= MOP_POST_GAMMA;
    if (ui_toolbar_is_on(&toolbar, btn_tonemap))
      post_effects |= MOP_POST_TONEMAP;
    if (ui_toolbar_is_on(&toolbar, btn_vignette))
      post_effects |= MOP_POST_VIGNETTE;
    if (ui_toolbar_is_on(&toolbar, btn_fog))
      post_effects |= MOP_POST_FOG;
    mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_POST_EFFECTS,
                                            .value = post_effects});

    /* Shading / render mode — send via MOP events */
    if (ui_toolbar_is_on(&toolbar, btn_smooth)) {
      mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_SHADING,
                                              .value = MOP_SHADING_SMOOTH});
      mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_RENDER_MODE,
                                              .value = MOP_RENDER_SOLID});
    }
    if (ui_toolbar_is_on(&toolbar, btn_flat)) {
      mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_SHADING,
                                              .value = MOP_SHADING_FLAT});
      mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_RENDER_MODE,
                                              .value = MOP_RENDER_SOLID});
    }
    if (ui_toolbar_is_on(&toolbar, btn_wire)) {
      mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_RENDER_MODE,
                                              .value = MOP_RENDER_WIREFRAME});
    }

    /* ---- Update marker positions ---- */
    for (int i = 0; i < 3; i++) {
      mop_mesh_set_position(
          marker_meshes[i],
          (MopVec3){emitters[i].px, emitters[i].py, emitters[i].pz});
    }

    /* ================================================================
     * APP SIMULATION STEP
     * ================================================================ */

    if (!paused) {
      sim_time += dt;

      for (int i = 0; i < 3; i++)
        sim_emitter_update(&emitters[i], dt);
    }

    /* ---- Camera basis for billboard orientation ---- */
    MopVec3 cam_eye = mop_viewport_get_camera_eye(vp);
    MopVec3 cam_target = mop_viewport_get_camera_target(vp);
    MopVec3 fwd = mop_vec3_normalize(mop_vec3_sub(cam_target, cam_eye));
    MopVec3 world_up = {0, 1, 0};
    MopVec3 cam_right = mop_vec3_normalize(mop_vec3_cross(fwd, world_up));
    MopVec3 cam_up = mop_vec3_cross(cam_right, fwd);

    /* ---- Scatter particles into voxel grid ---- */
    voxel_grid_scatter(voxel_grid, emitters);

    /* ---- Generate display geometry ---- */
    uint32_t disp_vc = 0, disp_ic = 0;
    MopBlendMode disp_blend = MOP_BLEND_ADDITIVE;

    switch (display_mode) {
    case DISPLAY_BILLBOARD:
      gen_billboards(emitters, cam_right, cam_up, disp_verts, disp_idx,
                     &disp_vc, &disp_ic);
      disp_blend = MOP_BLEND_ADDITIVE;
      break;
    case DISPLAY_VOLUME:
      gen_volume_slices(voxel_grid, cam_eye, cam_target, disp_verts, disp_idx,
                        &disp_vc, &disp_ic);
      disp_blend = MOP_BLEND_ALPHA;
      break;
    case DISPLAY_ISOSURFACE:
      gen_isosurface(voxel_grid, 0.5f, disp_verts, disp_idx, &disp_vc,
                     &disp_ic);
      disp_blend = MOP_BLEND_OPAQUE;
      break;
    case DISPLAY_POINTCLOUD:
      gen_pointcloud(voxel_grid, 0.3f, cam_right, cam_up, disp_verts, disp_idx,
                     &disp_vc, &disp_ic);
      disp_blend = MOP_BLEND_ADDITIVE;
      break;
    }

    /* ---- Submit to MOP ---- */
    if (disp_vc > 0 && disp_ic > 0) {
      if (!display_mesh) {
        display_mesh =
            mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = disp_verts,
                                                     .vertex_count = disp_vc,
                                                     .indices = disp_idx,
                                                     .index_count = disp_ic,
                                                     .object_id = 10});
        if (display_mesh) {
          mop_mesh_set_blend_mode(display_mesh, disp_blend);
          mop_mesh_set_opacity(display_mesh,
                               disp_blend == MOP_BLEND_ALPHA ? 0.8f : 1.0f);
        }
      } else {
        mop_mesh_update_geometry(display_mesh, vp, disp_verts, disp_vc,
                                 disp_idx, disp_ic);
        mop_mesh_set_blend_mode(display_mesh, disp_blend);
        mop_mesh_set_opacity(display_mesh,
                             disp_blend == MOP_BLEND_ALPHA ? 0.8f : 1.0f);
      }
    } else if (display_mesh) {
      mop_viewport_remove_mesh(vp, display_mesh);
      display_mesh = NULL;
    }

    /* ---- Set simulation time on viewport ---- */
    mop_viewport_set_time(vp, sim_time);

    /* ---- MOP: render ---- */
    mop_viewport_render(vp);

    /* ---- Blit to SDL ---- */
    int fb_w, fb_h;
    const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
    if (px && tex) {
      SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
      SDL_RenderClear(sdl_renderer);
      SDL_RenderTexture(sdl_renderer, tex, NULL, NULL);

      /* Draw sidebar on top */
      ui_toolbar_render(&toolbar, sdl_renderer, win_h);

      SDL_RenderPresent(sdl_renderer);
    }
  }

  /* ---- Cleanup ---- */
  if (display_mesh)
    mop_viewport_remove_mesh(vp, display_mesh);
  for (int i = 0; i < 3; i++) {
    mop_viewport_remove_mesh(vp, marker_meshes[i]);
    sim_emitter_free(&emitters[i]);
  }
  free(disp_verts);
  free(disp_idx);
  free(voxel_grid);
  SDL_DestroyTexture(tex);
  mop_viewport_destroy(vp);
  SDL_DestroyRenderer(sdl_renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

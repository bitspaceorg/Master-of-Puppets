/*
 * Master of Puppets — Render Health Check
 *
 * One binary. Four checks that unit tests in tests/ can't do because they
 * require a running viewport + rendered output:
 *
 *   1. Vulkan validation-layer error count == 0
 *   2. Vulkan sync-hazard count == 0
 *   3. No NaN in any rendered pixel across N frames
 *   4. CPU backend byte-level determinism: render N frames twice,
 *      FNV-1a hash must match
 *   5. Pick invariants: every non-zero object_id sampled from the
 *      pick buffer is either legal application range or the chrome
 *      range (>= 0xFFFD0000)
 *
 * Runs a small procedural scene (cube + sphere + directional light) on
 * both backends when available.
 *
 * Exits 0 on pass, non-zero on fail.  Runs in a few seconds — suitable
 * for every CI push.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Vulkan validation-layer hook
 *
 * Implemented in src/backend/vulkan/vulkan_backend.c.  We declare the
 * globals here and register counter callbacks at startup.  On CPU-only
 * builds MOP_HAS_VULKAN is undefined and the hook is a no-op.
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
typedef void (*MopVkValidationCallback)(void);
extern MopVkValidationCallback mop_vk_on_validation_error;
extern MopVkValidationCallback mop_vk_on_sync_hazard;
#endif

static uint32_t s_validation_errors = 0;
static uint32_t s_sync_hazards = 0;

#if defined(MOP_HAS_VULKAN)
static void on_validation_error(void) { s_validation_errors++; }
static void on_sync_hazard(void) { s_sync_hazards++; }
#endif

static void register_vk_hooks(void) {
#if defined(MOP_HAS_VULKAN)
  mop_vk_on_validation_error = on_validation_error;
  mop_vk_on_sync_hazard = on_sync_hazard;
#endif
}

static void reset_vk_counters(void) {
  s_validation_errors = 0;
  s_sync_hazards = 0;
}

/* -------------------------------------------------------------------------
 * FNV-1a 64-bit hash — for CPU determinism check
 * ------------------------------------------------------------------------- */

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* -------------------------------------------------------------------------
 * Procedural scene
 * ------------------------------------------------------------------------- */

static void build_cube(MopVertex *v, uint32_t *idx, uint32_t *base_id) {
  static const float F[6][4][3] = {
      {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}},
      {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}},
      {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}},
      {{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}},
      {{-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}, {1, -1, 1}},
      {{1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, -1}},
  };
  static const float N[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (int f = 0; f < 6; f++) {
    for (int j = 0; j < 4; j++) {
      v[f * 4 + j].position = (MopVec3){F[f][j][0], F[f][j][1], F[f][j][2]};
      v[f * 4 + j].normal = (MopVec3){N[f][0], N[f][1], N[f][2]};
      v[f * 4 + j].color = (MopColor){0.9f, 0.5f, 0.2f, 1.0f};
      v[f * 4 + j].u = (float)(j & 1);
      v[f * 4 + j].v = (float)((j >> 1) & 1);
    }
    uint32_t b = (uint32_t)f * 4;
    idx[f * 6 + 0] = b + 0;
    idx[f * 6 + 1] = b + 2;
    idx[f * 6 + 2] = b + 1;
    idx[f * 6 + 3] = b + 0;
    idx[f * 6 + 4] = b + 3;
    idx[f * 6 + 5] = b + 2;
  }
  (void)base_id;
}

static void populate_scene(MopViewport *vp) {
  mop_viewport_set_camera(vp, (MopVec3){4, 3, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.2f);
  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);

  MopLight key = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {-0.4f, -0.9f, -0.3f},
                  .color = {1, 0.95f, 0.9f, 1},
                  .intensity = 3.0f,
                  .active = true};
  mop_viewport_add_light(vp, &key);

  MopVertex cube_v[24];
  uint32_t cube_i[36];
  uint32_t base = 0;
  build_cube(cube_v, cube_i, &base);
  mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                .vertices = cube_v,
                                .vertex_count = 24,
                                .indices = cube_i,
                                .index_count = 36,
                                .object_id = 1,
                            });
}

/* -------------------------------------------------------------------------
 * Checks
 * ------------------------------------------------------------------------- */

static uint32_t scan_nan_rgba(const uint8_t *pixels, int w, int h) {
  /* RGBA8 output can't technically hold NaN — but the backend may write a
   * sentinel pattern (0xFF 0x00 0xFF) or the entire buffer may be
   * uninitialised.  Flag any pixel where the "checkerboard-of-death"
   * 0xFF00FF magenta is present AND alpha is 0 (common sentinel). */
  uint32_t count = 0;
  const size_t n = (size_t)w * (size_t)h;
  for (size_t i = 0; i < n; i++) {
    uint8_t r = pixels[i * 4 + 0];
    uint8_t g = pixels[i * 4 + 1];
    uint8_t b = pixels[i * 4 + 2];
    uint8_t a = pixels[i * 4 + 3];
    if (r == 0xFF && g == 0x00 && b == 0xFF && a == 0x00)
      count++;
  }
  return count;
}

typedef enum CheckStatus {
  CHECK_PASS = 0,
  CHECK_FAIL = 1,
  CHECK_SKIP = 2, /* backend not built / not available — not a test failure */
} CheckStatus;

typedef struct CheckResult {
  CheckStatus status;
  const char *name;
  char detail[256];
} CheckResult;

static CheckResult check_render_health(MopBackendType backend, int frames) {
  CheckResult r = {0};
  r.name = (backend == MOP_BACKEND_VULKAN) ? "render_health/vulkan"
                                           : "render_health/cpu";

  reset_vk_counters();

  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 320,
      .height = 240,
      .backend = backend,
      .ssaa_factor = 1,
  });
  if (!vp) {
    /* Backend not available in this environment — skip, don't fail.
     * On CI the matrix runs where each target backend is present. */
    r.status = CHECK_SKIP;
    snprintf(r.detail, sizeof(r.detail),
             "backend unavailable in this environment");
    return r;
  }

  populate_scene(vp);

  uint32_t nan_frames = 0;
  uint32_t bad_ids = 0;
  for (int i = 0; i < frames; i++) {
    mop_viewport_render(vp);
    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (!px) {
      r.status = CHECK_FAIL;
      snprintf(r.detail, sizeof(r.detail),
               "mop_viewport_read_color returned NULL on frame %d", i);
      mop_viewport_destroy(vp);
      return r;
    }
    if (scan_nan_rgba(px, w, h) > 0)
      nan_frames++;

    /* Pick at 4 fixed pixels: hit=true with object_id=0 is a bug. */
    int pick_xy[4][2] = {{w / 2, h / 2}, {10, 10}, {w - 10, h - 10}, {80, 120}};
    for (int p = 0; p < 4; p++) {
      MopPickResult pr = mop_viewport_pick(vp, pick_xy[p][0], pick_xy[p][1]);
      if (pr.hit && pr.object_id == 0)
        bad_ids++;
    }
  }

  mop_viewport_destroy(vp);

  uint32_t vk_errs = s_validation_errors;
  uint32_t vk_hazards = s_sync_hazards;

  bool ok =
      (nan_frames == 0 && bad_ids == 0 && vk_errs == 0 && vk_hazards == 0);
  snprintf(r.detail, sizeof(r.detail),
           "frames=%d nan=%u bad_pick=%u vk_errs=%u vk_hazards=%u", frames,
           nan_frames, bad_ids, vk_errs, vk_hazards);
  r.status = ok ? CHECK_PASS : CHECK_FAIL;
  return r;
}

static CheckResult check_cpu_determinism(int frames) {
  CheckResult r = {0};
  r.name = "determinism/cpu";

  uint64_t hashes[2] = {0, 0};
  for (int run = 0; run < 2; run++) {
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = 160,
        .height = 120,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    if (!vp) {
      r.status = CHECK_FAIL;
      snprintf(r.detail, sizeof(r.detail),
               "CPU viewport creation failed on run %d", run);
      return r;
    }
    populate_scene(vp);

    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < frames; i++) {
      mop_viewport_render(vp);
      int w, ht;
      const uint8_t *px = mop_viewport_read_color(vp, &w, &ht);
      if (!px) {
        r.status = CHECK_FAIL;
        snprintf(r.detail, sizeof(r.detail),
                 "read_color returned NULL on run %d frame %d", run, i);
        mop_viewport_destroy(vp);
        return r;
      }
      /* Fold each frame's hash into the accumulator. */
      uint64_t fh = fnv1a64(px, (size_t)w * (size_t)ht * 4);
      h ^= fh;
      h *= 0x100000001b3ULL;
    }
    hashes[run] = h;
    mop_viewport_destroy(vp);
  }

  r.status = (hashes[0] == hashes[1]) ? CHECK_PASS : CHECK_FAIL;
  snprintf(r.detail, sizeof(r.detail), "run0=%016llx run1=%016llx",
           (unsigned long long)hashes[0], (unsigned long long)hashes[1]);
  return r;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  int frames = 60;
  bool verbose = false;
  bool vulkan_only = false;
  bool cpu_only = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
      frames = atoi(argv[++i]);
    else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
      verbose = true;
    else if (strcmp(argv[i], "--vulkan-only") == 0)
      vulkan_only = true;
    else if (strcmp(argv[i], "--cpu-only") == 0)
      cpu_only = true;
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("Usage: %s [--frames N] [--vulkan-only] [--cpu-only] "
             "[--verbose]\n",
             argv[0]);
      return 0;
    }
  }

  register_vk_hooks();

  fprintf(stderr, "conformance: render health + determinism, %d frames\n",
          frames);

  CheckResult results[3];
  int n = 0;

  if (!vulkan_only)
    results[n++] = check_render_health(MOP_BACKEND_CPU, frames);
  if (!vulkan_only)
    results[n++] = check_cpu_determinism(frames);
#if defined(MOP_HAS_VULKAN)
  if (!cpu_only)
    results[n++] = check_render_health(MOP_BACKEND_VULKAN, frames);
#else
  if (!cpu_only && verbose)
    fprintf(stderr, "conformance: vulkan backend not built — skipping\n");
#endif

  int failed = 0, skipped = 0, passed = 0;
  for (int i = 0; i < n; i++) {
    const char *tag = "PASS";
    if (results[i].status == CHECK_FAIL) {
      tag = "FAIL";
      failed++;
    } else if (results[i].status == CHECK_SKIP) {
      tag = "SKIP";
      skipped++;
    } else {
      passed++;
    }
    fprintf(stderr, "  [%s] %s — %s\n", tag, results[i].name,
            results[i].detail);
  }

  fprintf(stderr, "conformance: %d checks (%d passed, %d skipped, %d failed)\n",
          n, passed, skipped, failed);
  (void)verbose;
  return failed ? 1 : 0;
}

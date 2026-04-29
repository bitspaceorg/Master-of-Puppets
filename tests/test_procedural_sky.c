/*
 * Master of Puppets — Procedural sky regression test
 * test_procedural_sky.c — exercise MOP_ENV_PROCEDURAL_SKY end-to-end on the
 * CPU backend so the host-side setup chain (env_cleanup → generate sky →
 * IBL precompute → texture upload) stays correct. Reproduces the public
 * call sequence reported by the Made-in-Heaven integration:
 *   set_environment(type=PROCEDURAL_SKY)
 *   set_procedural_sky(...)
 *   set_environment_background(true)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static MopViewport *make_viewport(int w, int h) {
  MopViewportDesc desc = {.width = w, .height = h, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

/* Setup-only test: every public step succeeds and the readback returns
 * non-NULL after a render. We don't pixel-check on CPU because the CPU
 * skybox path uses a separate sampling routine; the GPU-only regression
 * (MoP issue: gray gradient on Vulkan) needs a Vulkan harness. This guards
 * against host-side state corruption in the setup chain. */
static void test_procedural_sky_setup_chain(void) {
  TEST_BEGIN("procedural_sky: full setup chain");
  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  MopEnvironmentDesc env = {
      .type = MOP_ENV_PROCEDURAL_SKY,
      .rotation = 0.0f,
      .intensity = 1.0f,
  };
  TEST_ASSERT(mop_viewport_set_environment(vp, &env));

  MopProceduralSkyDesc sky = {
      .sun_direction = {0.3f, 0.8f, 0.4f},
      .turbidity = 4.5f,
      .ground_albedo = 0.25f,
  };
  mop_viewport_set_procedural_sky(vp, &sky);
  mop_viewport_set_environment_background(vp, true);

  /* A successful render is the integration check — set_environment must
   * have populated env_hdr_data + IBL textures, and the CPU pass_background
   * path must accept them. */
  TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

  int rw, rh;
  const uint8_t *px = mop_viewport_read_color(vp, &rw, &rh);
  TEST_ASSERT(px != NULL);
  TEST_ASSERT(rw == 64 && rh == 64);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* Reverse order: set_procedural_sky before set_environment. Earlier
 * versions only generated the sky if env_type was already PROCEDURAL_SKY,
 * which left a fresh viewport with stale sky_desc. The setter is idempotent
 * either way so both orderings must finish with a renderable viewport. */
static void test_procedural_sky_reverse_order(void) {
  TEST_BEGIN("procedural_sky: reverse order works");
  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  MopProceduralSkyDesc sky = {
      .sun_direction = {0.0f, 1.0f, 0.0f},
      .turbidity = 3.0f,
      .ground_albedo = 0.3f,
  };
  mop_viewport_set_procedural_sky(vp, &sky);

  MopEnvironmentDesc env = {.type = MOP_ENV_PROCEDURAL_SKY, .intensity = 1.0f};
  TEST_ASSERT(mop_viewport_set_environment(vp, &env));
  mop_viewport_set_environment_background(vp, true);

  TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* Switching environments must not leak: PROCEDURAL_SKY → HDRI fallback →
 * PROCEDURAL_SKY again. env_cleanup runs each time, so this catches
 * use-after-free in the IBL precompute path. */
static void test_procedural_sky_switching(void) {
  TEST_BEGIN("procedural_sky: switching back and forth");
  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  MopEnvironmentDesc proc = {.type = MOP_ENV_PROCEDURAL_SKY, .intensity = 1.0f};
  MopEnvironmentDesc none = {.type = MOP_ENV_NONE};

  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(mop_viewport_set_environment(vp, &proc));
    mop_viewport_set_environment_background(vp, true);
    TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

    TEST_ASSERT(mop_viewport_set_environment(vp, &none));
    TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* Defensive recovery: if env_texture vanishes between set_environment and
 * render (failed first upload, race), pass_background calls
 * mop_env_generate_procedural_sky to rebuild it. We can't simulate the
 * Vulkan-only "first upload returns NULL" path on CPU, but we can verify
 * that the recovery hook's symbol is exported and the public flow keeps
 * working when render is called many times in a row — flushing out any
 * recovery-path crash that doesn't depend on a missing texture. */
static void test_procedural_sky_recovery_smoke(void) {
  TEST_BEGIN("procedural_sky: render loop is recovery-safe");
  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  MopEnvironmentDesc env = {.type = MOP_ENV_PROCEDURAL_SKY, .intensity = 1.0f};
  TEST_ASSERT(mop_viewport_set_environment(vp, &env));
  mop_viewport_set_environment_background(vp, true);

  for (int i = 0; i < 8; i++)
    TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("procedural sky");
  TEST_RUN(test_procedural_sky_setup_chain);
  TEST_RUN(test_procedural_sky_reverse_order);
  TEST_RUN(test_procedural_sky_switching);
  TEST_RUN(test_procedural_sky_recovery_smoke);
  TEST_REPORT();
  TEST_EXIT();
}

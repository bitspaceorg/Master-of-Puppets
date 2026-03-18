/*
 * Master of Puppets — Render Graph Tests
 * test_render_graph.c — Phase 1A: DAG pass management, resource declarations
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* Access render graph internals for direct testing */
#include "core/render_graph.h"

/* Execution tracking */
static int s_exec_order[16];
static int s_exec_count = 0;

static void pass_a(MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  s_exec_order[s_exec_count++] = 0;
}

static void pass_b(MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  s_exec_order[s_exec_count++] = 1;
}

static void pass_c(MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  s_exec_order[s_exec_count++] = 2;
}

/* ---- Tests ---- */

static void test_rg_clear(void) {
  TEST_BEGIN("rg_clear");
  MopRenderGraph rg;
  rg.pass_count = 42;
  mop_rg_clear(&rg);
  TEST_ASSERT(rg.pass_count == 0);
  TEST_END();
}

static void test_rg_add_pass(void) {
  TEST_BEGIN("rg_add_pass");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass p = {.name = "test_pass", .execute = pass_a};
  uint32_t idx = mop_rg_add_pass(&rg, &p);
  TEST_ASSERT(idx == 0);
  TEST_ASSERT(rg.pass_count == 1);

  /* Name stored correctly */
  TEST_ASSERT(strcmp(rg.passes[0].name, "test_pass") == 0);
  TEST_END();
}

static void test_rg_add_multiple_passes(void) {
  TEST_BEGIN("rg_add_multiple_passes");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass pa = {.name = "A", .execute = pass_a};
  MopRgPass pb = {.name = "B", .execute = pass_b};
  MopRgPass pc = {.name = "C", .execute = pass_c};

  uint32_t ia = mop_rg_add_pass(&rg, &pa);
  uint32_t ib = mop_rg_add_pass(&rg, &pb);
  uint32_t ic = mop_rg_add_pass(&rg, &pc);

  TEST_ASSERT(ia == 0);
  TEST_ASSERT(ib == 1);
  TEST_ASSERT(ic == 2);
  TEST_ASSERT(rg.pass_count == 3);
  TEST_END();
}

static void test_rg_execute_order(void) {
  TEST_BEGIN("rg_execute_order");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass pa = {.name = "A", .execute = pass_a};
  MopRgPass pb = {.name = "B", .execute = pass_b};
  MopRgPass pc = {.name = "C", .execute = pass_c};
  mop_rg_add_pass(&rg, &pa);
  mop_rg_add_pass(&rg, &pb);
  mop_rg_add_pass(&rg, &pc);

  s_exec_count = 0;
  mop_rg_execute(&rg, NULL);

  /* Linear execution: A → B → C */
  TEST_ASSERT(s_exec_count == 3);
  TEST_ASSERT(s_exec_order[0] == 0);
  TEST_ASSERT(s_exec_order[1] == 1);
  TEST_ASSERT(s_exec_order[2] == 2);
  TEST_END();
}

static void test_rg_resource_declarations(void) {
  TEST_BEGIN("rg_resource_declarations");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgResourceId reads[] = {MOP_RG_RES_DEPTH};
  MopRgResourceId writes[] = {MOP_RG_RES_COLOR_HDR, MOP_RG_RES_PICK};
  MopRgPass p = {
      .name = "scene",
      .execute = pass_a,
      .read_count = 1,
      .write_count = 2,
  };
  memcpy(p.reads, reads, sizeof(reads));
  memcpy(p.writes, writes, sizeof(writes));

  uint32_t idx = mop_rg_add_pass(&rg, &p);
  TEST_ASSERT(idx == 0);
  TEST_ASSERT(rg.passes[0].read_count == 1);
  TEST_ASSERT(rg.passes[0].reads[0] == MOP_RG_RES_DEPTH);
  TEST_ASSERT(rg.passes[0].write_count == 2);
  TEST_ASSERT(rg.passes[0].writes[0] == MOP_RG_RES_COLOR_HDR);
  TEST_ASSERT(rg.passes[0].writes[1] == MOP_RG_RES_PICK);
  TEST_END();
}

static void test_rg_max_passes(void) {
  TEST_BEGIN("rg_max_passes");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass p = {.name = "fill", .execute = pass_a};

  /* Fill to max */
  for (int i = 0; i < MOP_RG_MAX_PASSES; i++) {
    uint32_t idx = mop_rg_add_pass(&rg, &p);
    TEST_ASSERT(idx == (uint32_t)i);
  }

  /* One more should fail */
  uint32_t overflow = mop_rg_add_pass(&rg, &p);
  TEST_ASSERT(overflow == UINT32_MAX);
  TEST_ASSERT(rg.pass_count == MOP_RG_MAX_PASSES);
  TEST_END();
}

static void test_rg_empty_execute(void) {
  TEST_BEGIN("rg_empty_execute");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  s_exec_count = 0;
  mop_rg_execute(&rg, NULL); /* should not crash */
  TEST_ASSERT(s_exec_count == 0);
  TEST_END();
}

static void test_rg_user_data(void) {
  TEST_BEGIN("rg_user_data");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  int magic = 42;
  MopRgPass p = {
      .name = "with_data",
      .execute = pass_a,
      .user_data = &magic,
  };
  mop_rg_add_pass(&rg, &p);
  TEST_ASSERT(rg.passes[0].user_data == &magic);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("render_graph");

  TEST_RUN(test_rg_clear);
  TEST_RUN(test_rg_add_pass);
  TEST_RUN(test_rg_add_multiple_passes);
  TEST_RUN(test_rg_execute_order);
  TEST_RUN(test_rg_resource_declarations);
  TEST_RUN(test_rg_max_passes);
  TEST_RUN(test_rg_empty_execute);
  TEST_RUN(test_rg_user_data);

  TEST_REPORT();
  TEST_EXIT();
}

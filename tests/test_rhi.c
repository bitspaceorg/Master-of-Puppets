/*
 * Master of Puppets — RHI Tests
 * test_rhi.c — Backend resolution, CPU always available, NULL for unsupported
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/rhi.h"
#include "test_harness.h"
#include <mop/backend.h>
#include <mop/types.h>

static void test_cpu_always_available(void) {
  TEST_BEGIN("cpu_backend_always_available");
  const MopRhiBackend *cpu = mop_rhi_get_backend(MOP_BACKEND_CPU);
  TEST_ASSERT(cpu != NULL);
  TEST_ASSERT(strcmp(cpu->name, "cpu") == 0);
  TEST_END();
}

static void test_auto_resolves_to_valid(void) {
  TEST_BEGIN("auto_resolves_to_valid_backend");
  const MopRhiBackend *b = mop_rhi_get_backend(MOP_BACKEND_AUTO);
  TEST_ASSERT(b != NULL);
  TEST_ASSERT(b->name != NULL);
  TEST_END();
}

static void test_unsupported_returns_null(void) {
  TEST_BEGIN("unsupported_backend_returns_null");
  /* Without MOP_HAS_OPENGL/VULKAN, these should return NULL */
#if !defined(MOP_HAS_OPENGL)
  const MopRhiBackend *gl = mop_rhi_get_backend(MOP_BACKEND_OPENGL);
  TEST_ASSERT(gl == NULL);
#endif
#if !defined(MOP_HAS_VULKAN)
  const MopRhiBackend *vk = mop_rhi_get_backend(MOP_BACKEND_VULKAN);
  TEST_ASSERT(vk == NULL);
#endif
  TEST_END();
}

static void test_backend_name(void) {
  TEST_BEGIN("backend_name_strings");
  TEST_ASSERT(strcmp(mop_backend_name(MOP_BACKEND_CPU), "cpu") == 0);
  TEST_ASSERT(strcmp(mop_backend_name(MOP_BACKEND_OPENGL), "opengl") == 0);
  TEST_ASSERT(strcmp(mop_backend_name(MOP_BACKEND_VULKAN), "vulkan") == 0);
  TEST_ASSERT(strcmp(mop_backend_name(MOP_BACKEND_AUTO), "auto") == 0);
  /* Invalid type returns "unknown" */
  TEST_ASSERT(strcmp(mop_backend_name((MopBackendType)99), "unknown") == 0);
  TEST_END();
}

static void test_cpu_all_ptrs_non_null(void) {
  TEST_BEGIN("cpu_backend_all_function_pointers");
  const MopRhiBackend *cpu = mop_rhi_get_backend(MOP_BACKEND_CPU);
  TEST_ASSERT(cpu->device_create != NULL);
  TEST_ASSERT(cpu->device_destroy != NULL);
  TEST_ASSERT(cpu->buffer_create != NULL);
  TEST_ASSERT(cpu->buffer_destroy != NULL);
  TEST_ASSERT(cpu->framebuffer_create != NULL);
  TEST_ASSERT(cpu->framebuffer_destroy != NULL);
  TEST_ASSERT(cpu->framebuffer_resize != NULL);
  TEST_ASSERT(cpu->frame_begin != NULL);
  TEST_ASSERT(cpu->frame_end != NULL);
  TEST_ASSERT(cpu->draw != NULL);
  TEST_ASSERT(cpu->pick_read_id != NULL);
  TEST_ASSERT(cpu->pick_read_depth != NULL);
  TEST_ASSERT(cpu->framebuffer_read_color != NULL);
  TEST_END();
}

static void test_cpu_device_lifecycle(void) {
  TEST_BEGIN("cpu_device_create_destroy");
  const MopRhiBackend *cpu = mop_rhi_get_backend(MOP_BACKEND_CPU);
  MopRhiDevice *dev = cpu->device_create();
  TEST_ASSERT(dev != NULL);
  cpu->device_destroy(dev);
  TEST_END();
}

static void test_cpu_buffer_lifecycle(void) {
  TEST_BEGIN("cpu_buffer_create_destroy");
  const MopRhiBackend *cpu = mop_rhi_get_backend(MOP_BACKEND_CPU);
  MopRhiDevice *dev = cpu->device_create();
  float data[] = {1.0f, 2.0f, 3.0f};
  MopRhiBufferDesc desc = {.data = data, .size = sizeof(data)};
  MopRhiBuffer *buf = cpu->buffer_create(dev, &desc);
  TEST_ASSERT(buf != NULL);
  cpu->buffer_destroy(dev, buf);
  cpu->device_destroy(dev);
  TEST_END();
}

static void test_cpu_framebuffer_lifecycle(void) {
  TEST_BEGIN("cpu_framebuffer_create_resize_destroy");
  const MopRhiBackend *cpu = mop_rhi_get_backend(MOP_BACKEND_CPU);
  MopRhiDevice *dev = cpu->device_create();
  MopRhiFramebufferDesc desc = {.width = 64, .height = 64};
  MopRhiFramebuffer *fb = cpu->framebuffer_create(dev, &desc);
  TEST_ASSERT(fb != NULL);
  cpu->framebuffer_resize(dev, fb, 128, 128);
  int w = 0, h = 0;
  const uint8_t *pixels = cpu->framebuffer_read_color(dev, fb, &w, &h);
  TEST_ASSERT(pixels != NULL);
  TEST_ASSERT(w == 128);
  TEST_ASSERT(h == 128);
  cpu->framebuffer_destroy(dev, fb);
  cpu->device_destroy(dev);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("rhi");

  TEST_RUN(test_cpu_always_available);
  TEST_RUN(test_auto_resolves_to_valid);
  TEST_RUN(test_unsupported_returns_null);
  TEST_RUN(test_backend_name);
  TEST_RUN(test_cpu_all_ptrs_non_null);
  TEST_RUN(test_cpu_device_lifecycle);
  TEST_RUN(test_cpu_buffer_lifecycle);
  TEST_RUN(test_cpu_framebuffer_lifecycle);

  TEST_REPORT();
  TEST_EXIT();
}

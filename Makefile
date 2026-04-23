# Master of Puppets — Build System
# Pure Make.  Nix provides the toolchain.
#
# Usage:
#   make              Build the static library
#   make lib          Build only the static library (same as make)
#   make clean        Remove build artifacts
#   make install      Install library and headers to PREFIX
#
# Examples live in examples/ with their own Makefile.

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
CC       ?= cc
AR       ?= ar
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wno-unused-parameter -Wno-missing-field-initializers \
            -fno-common -fno-strict-aliasing \
            -Iinclude -Isrc -Ithird_party/stb -Ithird_party/tinyexr
CXX      ?= c++
CXXFLAGS := -std=c++11 -Wall -Wextra -Wno-unused-parameter \
            -Iinclude -Isrc -Ithird_party/tinyexr

# Optimization — -Og preserves debuggability while avoiding -O0's
# catastrophic performance on math-heavy code (GGX specular, etc.)
ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
  CXXFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -Og -g
  CXXFLAGS += -Og -g
endif

# Sanitizer support: make SANITIZE=asan  or  make SANITIZE=ubsan
ifdef SANITIZE
ifeq ($(SANITIZE),asan)
CFLAGS += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
endif
ifeq ($(SANITIZE),ubsan)
CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=undefined
endif
ifeq ($(SANITIZE),tsan)
CFLAGS += -fsanitize=thread -fno-omit-frame-pointer
CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer
LDFLAGS += -fsanitize=thread
endif
endif

# -----------------------------------------------------------------------------
# Platform detection
# -----------------------------------------------------------------------------
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DMOP_PLATFORM_MACOS=1
endif
ifeq ($(UNAME_S),Linux)
  # _GNU_SOURCE exposes POSIX/BSD/GNU symbols (strdup, usleep, sched_yield,
  # pthread_mutexattr_settype) under strict -std=c11 on glibc. Darwin
  # headers are permissive by default; glibc isn't.
  CFLAGS += -DMOP_PLATFORM_LINUX=1 -D_GNU_SOURCE
endif

# -----------------------------------------------------------------------------
# Source files
# -----------------------------------------------------------------------------
CORE_SRCS := \
  src/math/math.c \
  src/rhi/rhi.c \
  src/core/viewport.c \
  src/core/vertex_format.c \
  src/core/theme.c \
  src/core/light.c \
  src/core/decal.c \
  src/core/display.c \
  src/core/overlay.c \
  src/core/overlay_builtin.c \
  src/core/camera_object.c \
  src/core/environment.c \
  src/core/render_graph.c \
  src/core/thread_pool.c \
  src/rasterizer/rasterizer.c \
  src/rasterizer/rasterizer_mt.c \
  src/interact/gizmo.c \
  src/interact/camera.c \
  src/interact/input.c \
  src/interact/undo.c \
  src/interact/selection.c \
  src/interact/mesh_edit.c \
  src/loader/obj_loader.c \
  src/loader/mop_loader.c \
  src/loader/loader.c \
  src/loader/gltf_loader.c \
  src/util/log.c \
  src/util/profile.c \
  src/render/postprocess.c \
  src/query/query.c \
  src/query/camera_query.c \
  src/query/snapshot.c \
  src/query/spatial.c \
  src/util/stb_impl.c \
  src/util/miniz_impl.c \
  src/export/image_export.c \
  src/export/obj_export.c \
  src/export/scene_export.c \
  src/loader/mop_scene.c \
  src/core/material_graph.c \
  src/core/texture_pipeline.c \
  src/core/meshlet.c \
  src/render/shader_plugin.c \
  src/backend/cpu/cpu_backend.c

# C++ sources (tinyexr requires C++)
CXX_SRCS := src/util/tinyexr_impl.cc

# Optional: OpenGL 3.3 backend (requires GL headers and libraries)
ifdef MOP_ENABLE_OPENGL
  CORE_SRCS += src/backend/opengl/opengl_backend.c
  CFLAGS    += -DMOP_HAS_OPENGL=1
  ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -framework OpenGL
  else
    LDFLAGS += -lGL
  endif
endif

# Optional: Vulkan backend (requires Vulkan SDK / MoltenVK)
ifdef MOP_ENABLE_VULKAN
  CORE_SRCS += src/backend/vulkan/vulkan_backend.c \
               src/backend/vulkan/vulkan_pipeline.c \
               src/backend/vulkan/vulkan_memory.c \
               src/backend/vulkan/vulkan_suballoc.c
  CFLAGS    += -DMOP_HAS_VULKAN=1
  # Use pkg-config for Vulkan headers/libs if available (nix)
  VK_CFLAGS  := $(shell pkg-config --cflags vulkan 2>/dev/null)
  VK_LDFLAGS := $(shell pkg-config --libs vulkan 2>/dev/null)
  CFLAGS     += $(VK_CFLAGS)
  LDFLAGS    += $(if $(VK_LDFLAGS),$(VK_LDFLAGS),-lvulkan)
endif

# -----------------------------------------------------------------------------
# Build directories and outputs
# -----------------------------------------------------------------------------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib

CORE_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))
CXX_OBJS  := $(patsubst src/%.cc,$(OBJ_DIR)/%.o,$(CXX_SRCS))
LIB_OUT   := $(LIB_DIR)/libmop.a

# -----------------------------------------------------------------------------
# Default target — static library only
# -----------------------------------------------------------------------------
.PHONY: all lib clean install test torture tools shaders conformance conformance-run docs-check docs-check-code

all: lib

# -----------------------------------------------------------------------------
# Static library
# -----------------------------------------------------------------------------
lib: $(LIB_OUT)

$(LIB_OUT): $(CORE_OBJS) $(CXX_OBJS) | $(LIB_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: src/%.c | obj_dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cc | obj_dirs
	$(CXX) $(CXXFLAGS) -c $< -o $@

# -----------------------------------------------------------------------------
# Directory creation
# -----------------------------------------------------------------------------
obj_dirs:
	@mkdir -p $(OBJ_DIR)/math
	@mkdir -p $(OBJ_DIR)/rhi
	@mkdir -p $(OBJ_DIR)/core
	@mkdir -p $(OBJ_DIR)/rasterizer
	@mkdir -p $(OBJ_DIR)/interact
	@mkdir -p $(OBJ_DIR)/loader
	@mkdir -p $(OBJ_DIR)/util
	@mkdir -p $(OBJ_DIR)/query
	@mkdir -p $(OBJ_DIR)/export
	@mkdir -p $(OBJ_DIR)/render
	@mkdir -p $(OBJ_DIR)/backend/cpu
	@mkdir -p $(OBJ_DIR)/backend/opengl
	@mkdir -p $(OBJ_DIR)/backend/vulkan

$(LIB_DIR):
	@mkdir -p $@

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
PREFIX ?= /usr/local

install: lib
	install -d $(PREFIX)/lib
	find include/mop -type d -exec sh -c 'install -d "$(PREFIX)/$${1}"' _ {} \;
	find include/mop -name '*.h' -exec sh -c 'install -m 644 "$${1}" "$(PREFIX)/$${1}"' _ {} \;

# -----------------------------------------------------------------------------
# Tests
#
# Each test_*.c in tests/ is compiled as a standalone binary that links
# against libmop.a.  `make test` builds and runs them all.
# -----------------------------------------------------------------------------

TEST_DIR    := tests
TEST_BIN    := $(BUILD_DIR)/tests
TEST_SRCS   := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS   := $(patsubst $(TEST_DIR)/%.c,$(TEST_BIN)/%,$(TEST_SRCS))

# Platform-specific test link flags.
# Linux: --as-needed around -lstdc++ so the linker drops libstdc++ for tests
# that don't actually reference any C++ symbols (the Vulkan-gated stubs).
# Otherwise libstdc++'s static destructors run at exit inside a pure-C
# binary and have been observed to trip glibc's malloc check ('free():
# invalid size') on CI.
TEST_LDFLAGS := -lm -lpthread
ifeq ($(UNAME_S),Darwin)
  TEST_LDFLAGS += -lc++
else
  TEST_LDFLAGS += -Wl,--as-needed -lstdc++ -Wl,--no-as-needed
endif

$(TEST_BIN):
	@mkdir -p $@

$(TEST_BIN)/%: $(TEST_DIR)/%.c $(LIB_OUT) | $(TEST_BIN)
	$(CC) $(CFLAGS) -I$(TEST_DIR) $< -L$(LIB_DIR) -lmop $(TEST_LDFLAGS) $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		if $$t; then :; else failed=1; fi; \
	done; \
	if [ $$failed -ne 0 ]; then exit 1; fi

# Torture tests only (adversarial / long-running subset)
TORTURE_SRCS := $(wildcard $(TEST_DIR)/test_torture_*.c)
TORTURE_BINS := $(patsubst $(TEST_DIR)/%.c,$(TEST_BIN)/%,$(TORTURE_SRCS))

torture: $(TORTURE_BINS)
	@failed=0; \
	for t in $(TORTURE_BINS); do \
		if $$t; then :; else failed=1; fi; \
	done; \
	if [ $$failed -ne 0 ]; then exit 1; fi

# -----------------------------------------------------------------------------
# Conformance — render health check (see conformance/runner.c).
#
# Runs against a live viewport and asserts: no Vulkan validation errors,
# no sync hazards, no NaN pixels, CPU backend byte-level determinism,
# valid pick results. Completes in seconds; runs on every CI push.
# -----------------------------------------------------------------------------
conformance: lib
	$(MAKE) -C conformance MOP_ENABLE_VULKAN=$(MOP_ENABLE_VULKAN) \
	                       MOP_ENABLE_OPENGL=$(MOP_ENABLE_OPENGL)

conformance-run: conformance
	./build/conformance_runner --verbose

# -----------------------------------------------------------------------------
# Shader compilation (Vulkan GLSL -> SPIR-V -> embedded C arrays)
# Requires: glslc (from shaderc / Vulkan SDK)
# -----------------------------------------------------------------------------
shaders:
	@echo "Compiling Vulkan shaders..."
	cd src/backend/vulkan/shaders && ./compile.sh

# -----------------------------------------------------------------------------
# Tools
# -----------------------------------------------------------------------------
tools: lib
	$(MAKE) -C tools

# -----------------------------------------------------------------------------
# Docs checks
#
# Link validation + slug / frontmatter / path-vs-slug consistency lives in
# tools/docs/validate.py (also runs in the `docs-build` pre-commit hook
# wired by nix/utils/precommit.nix).  The code-block check is additive: it
# compiles every fenced ```c block that has int main( against libmop.a.
#
#   docs-check       both (requires `make` first for the code check)
#   docs-check-code  compile every fenced ```c block with int main(
# -----------------------------------------------------------------------------
docs-check: docs-check-code
	python3 tools/docs/validate.py

docs-check-code: lib
	python3 tools/docs/compile_blocks.py

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
            -Iinclude -Isrc

# Optimization
ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -O0 -g
endif

# -----------------------------------------------------------------------------
# Platform detection
# -----------------------------------------------------------------------------
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DMOP_PLATFORM_MACOS=1
endif
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DMOP_PLATFORM_LINUX=1
endif

# -----------------------------------------------------------------------------
# Source files
# -----------------------------------------------------------------------------
CORE_SRCS := \
  src/math/math.c \
  src/rhi/rhi.c \
  src/viewport/viewport.c \
  src/viewport/vertex_format.c \
  src/viewport/light.c \
  src/viewport/display.c \
  src/viewport/overlay.c \
  src/viewport/overlay_builtin.c \
  src/rasterizer/rasterizer.c \
  src/rasterizer/rasterizer_mt.c \
  src/gizmo/gizmo.c \
  src/camera/camera.c \
  src/input/input.c \
  src/loader/obj_loader.c \
  src/loader/mop_loader.c \
  src/log/log.c \
  src/profile/profile.c \
  src/undo/undo.c \
  src/particle/particle.c \
  src/water/water.c \
  src/postprocess/postprocess.c \
  src/backend/cpu/cpu_backend.c

# Optional: Lua config support (requires lua5.4 or luajit via pkg-config)
ifdef MOP_ENABLE_LUA
  CORE_SRCS += src/config/config.c
  CFLAGS    += -DMOP_HAS_LUA=1
  LUA_CFLAGS  := $(shell pkg-config --cflags lua5.4 2>/dev/null || \
                          pkg-config --cflags luajit 2>/dev/null || \
                          pkg-config --cflags lua 2>/dev/null)
  CFLAGS    += $(LUA_CFLAGS)
endif

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
               src/backend/vulkan/vulkan_memory.c
  CFLAGS    += -DMOP_HAS_VULKAN=1
  LDFLAGS   += -lvulkan
endif

# -----------------------------------------------------------------------------
# Build directories and outputs
# -----------------------------------------------------------------------------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib

CORE_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))
LIB_OUT   := $(LIB_DIR)/libmop.a

# -----------------------------------------------------------------------------
# Default target — static library only
# -----------------------------------------------------------------------------
.PHONY: all lib clean install test tools

all: lib

# -----------------------------------------------------------------------------
# Static library
# -----------------------------------------------------------------------------
lib: $(LIB_OUT)

$(LIB_OUT): $(CORE_OBJS) | $(LIB_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: src/%.c | obj_dirs
	$(CC) $(CFLAGS) -c $< -o $@

# -----------------------------------------------------------------------------
# Directory creation
# -----------------------------------------------------------------------------
obj_dirs:
	@mkdir -p $(OBJ_DIR)/math
	@mkdir -p $(OBJ_DIR)/rhi
	@mkdir -p $(OBJ_DIR)/viewport
	@mkdir -p $(OBJ_DIR)/rasterizer
	@mkdir -p $(OBJ_DIR)/gizmo
	@mkdir -p $(OBJ_DIR)/camera
	@mkdir -p $(OBJ_DIR)/input
	@mkdir -p $(OBJ_DIR)/config
	@mkdir -p $(OBJ_DIR)/log
	@mkdir -p $(OBJ_DIR)/loader
	@mkdir -p $(OBJ_DIR)/profile
	@mkdir -p $(OBJ_DIR)/undo
	@mkdir -p $(OBJ_DIR)/particle
	@mkdir -p $(OBJ_DIR)/water
	@mkdir -p $(OBJ_DIR)/postprocess
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
	install -d $(PREFIX)/lib $(PREFIX)/include/mop
	install -m 644 $(LIB_OUT) $(PREFIX)/lib/
	install -m 644 include/mop/*.h $(PREFIX)/include/mop/

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

# Platform-specific test link flags
TEST_LDFLAGS := -lm -lpthread

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

# -----------------------------------------------------------------------------
# Tools
# -----------------------------------------------------------------------------
tools: lib
	$(MAKE) -C tools

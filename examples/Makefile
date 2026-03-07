# Master of Puppets — Examples
# Builds example programs that link against libmop.a.
#
# Usage:
#   make              Build the interactive viewport
#   make run          Build and run the interactive viewport
#   make clean        Remove build artifacts
#
# The root library must be built first:  cd .. && make

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
          -Wno-unused-parameter -Wno-missing-field-initializers \
          -I../include -Icommon

ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -O2 -g
endif

# -----------------------------------------------------------------------------
# Library
# -----------------------------------------------------------------------------
LIB := ../build/lib/libmop.a

# -----------------------------------------------------------------------------
# Platform detection
# -----------------------------------------------------------------------------
UNAME_S := $(shell uname -s)

PLATFORM_LDFLAGS :=
ifeq ($(UNAME_S),Darwin)
  PLATFORM_LDFLAGS += -lc++
else
  PLATFORM_LDFLAGS += -lm -ldl -lstdc++
endif

# -----------------------------------------------------------------------------
# SDL3 detection via pkg-config
# -----------------------------------------------------------------------------
SDL3_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LDFLAGS := $(shell pkg-config --libs   sdl3 2>/dev/null)
HAS_SDL3     := $(if $(SDL3_LDFLAGS),1,)

# -----------------------------------------------------------------------------
# OpenGL detection (opt-in via MOP_ENABLE_OPENGL=1)
# -----------------------------------------------------------------------------
ifdef MOP_ENABLE_OPENGL
  CFLAGS += -DMOP_HAS_OPENGL=1
  ifeq ($(UNAME_S),Darwin)
    PLATFORM_LDFLAGS += -framework OpenGL
  else
    PLATFORM_LDFLAGS += -lGL -lEGL
  endif
endif

# -----------------------------------------------------------------------------
# Vulkan detection — auto via pkg-config, or opt-in via MOP_ENABLE_VULKAN=1
# -----------------------------------------------------------------------------
VK_CFLAGS  := $(shell pkg-config --cflags vulkan 2>/dev/null)
VK_LDFLAGS := $(shell pkg-config --libs vulkan 2>/dev/null)
HAS_VULKAN := $(if $(VK_LDFLAGS),1,)

ifdef MOP_ENABLE_VULKAN
  HAS_VULKAN := 1
endif

ifeq ($(HAS_VULKAN),1)
  CFLAGS += -DMOP_HAS_VULKAN=1 $(VK_CFLAGS)
  PLATFORM_LDFLAGS += $(if $(VK_LDFLAGS),$(VK_LDFLAGS),-lvulkan)
endif

# -----------------------------------------------------------------------------
# Build directories and outputs
# -----------------------------------------------------------------------------
BUILD_DIR       := build
INTERACTIVE_OUT := $(BUILD_DIR)/mop_viewport

# -----------------------------------------------------------------------------
# Shared header dependencies
# -----------------------------------------------------------------------------
COMMON_GEO := common/geometry.h
COMMON_SDL := common/sdl_harness.h
COMMON_UI  := common/ui_toolbar.h

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all interactive run clean

all: interactive

# -----------------------------------------------------------------------------
# Interactive viewport (requires SDL3)
# -----------------------------------------------------------------------------
interactive: $(INTERACTIVE_OUT)

$(INTERACTIVE_OUT): interactive.c $(COMMON_GEO) $(COMMON_SDL) $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(PLATFORM_LDFLAGS) -lm -o $@
else
	@echo "ERROR: SDL3 not found via pkg-config."
	@echo "       Enter the examples nix shell first:"
	@echo "         nix develop"
	@exit 1
endif

run: $(INTERACTIVE_OUT)
	MOP_BACKEND=vulkan ./$<

# -----------------------------------------------------------------------------
# Directory creation
# -----------------------------------------------------------------------------
$(BUILD_DIR):
	@mkdir -p $@

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)

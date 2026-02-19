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
  src/rasterizer/rasterizer.c \
  src/gizmo/gizmo.c \
  src/backend/cpu/cpu_backend.c

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
.PHONY: all lib clean install

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
	@mkdir -p $(OBJ_DIR)/backend/cpu

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

# Master of Puppets — Examples
# Builds example programs that link against libmop.a.
#
# Usage:
#   make              Build all examples
#   make headless     Build the headless PPM exporter
#   make interactive  Build the interactive SDL3 viewport
#   make particles    Build the interactive particle simulation (SDL3)
#   make water        Build the interactive water simulation (SDL3)
#   make showcase     Build the Phase 1 showcase demo (SDL3)
#   make run          Build and run the interactive viewport
#   make run-headless Build and run the headless PPM exporter
#   make run-showcase Build and run the Phase 1 showcase demo
#   make clean        Remove build artifacts
#
# The root library must be built first:  cd .. && make

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
          -Wno-unused-parameter -Wno-missing-field-initializers \
          -I../include

ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -O0 -g
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
ifeq ($(UNAME_S),Linux)
  PLATFORM_LDFLAGS += -lm -ldl
endif

# -----------------------------------------------------------------------------
# SDL3 detection via pkg-config
# -----------------------------------------------------------------------------
SDL3_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LDFLAGS := $(shell pkg-config --libs   sdl3 2>/dev/null)
HAS_SDL3     := $(if $(SDL3_LDFLAGS),1,)

# -----------------------------------------------------------------------------
# Lua detection via pkg-config (optional — for config file support)
# -----------------------------------------------------------------------------
LUA_CFLAGS  := $(shell pkg-config --cflags lua5.4 2>/dev/null || \
                        pkg-config --cflags lua 2>/dev/null)
LUA_LDFLAGS := $(shell pkg-config --libs lua5.4 2>/dev/null || \
                        pkg-config --libs lua 2>/dev/null)
HAS_LUA     := $(if $(LUA_LDFLAGS),1,)

ifeq ($(HAS_LUA),1)
  CFLAGS += -DMOP_HAS_LUA=1 $(LUA_CFLAGS)
endif

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
# Vulkan detection (opt-in via MOP_ENABLE_VULKAN=1)
# -----------------------------------------------------------------------------
ifdef MOP_ENABLE_VULKAN
  CFLAGS += -DMOP_HAS_VULKAN=1
  PLATFORM_LDFLAGS += -lvulkan
endif

# -----------------------------------------------------------------------------
# Build directories and outputs
# -----------------------------------------------------------------------------
BUILD_DIR       := build
HEADLESS_OUT    := $(BUILD_DIR)/mop_headless
INTERACTIVE_OUT := $(BUILD_DIR)/mop_viewport
PARTICLE_OUT    := $(BUILD_DIR)/mop_particles
WATER_OUT       := $(BUILD_DIR)/mop_water
COMPARE_OUT     := $(BUILD_DIR)/mop_compare
SHOWCASE_OUT    := $(BUILD_DIR)/mop_showcase

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all headless interactive particles water showcase compare clean run run-headless run-showcase

all: headless
ifeq ($(HAS_SDL3),1)
all: interactive particles water showcase
endif

# -----------------------------------------------------------------------------
# Headless example (no external deps beyond libmop)
# -----------------------------------------------------------------------------
headless: $(HEADLESS_OUT)

$(HEADLESS_OUT): headless.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB) $(PLATFORM_LDFLAGS) -o $@

run-headless: $(HEADLESS_OUT)
	./$<

# -----------------------------------------------------------------------------
# Interactive viewport (requires SDL3)
# -----------------------------------------------------------------------------
interactive: $(INTERACTIVE_OUT)

$(INTERACTIVE_OUT): rotating_cube.c $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(LUA_LDFLAGS) $(PLATFORM_LDFLAGS) -o $@
else
	@echo "ERROR: SDL3 not found via pkg-config."
	@echo "       Enter the examples nix shell first:"
	@echo "         nix develop"
	@exit 1
endif

run: $(INTERACTIVE_OUT)
	./$<

# -----------------------------------------------------------------------------
# Particle simulation (requires SDL3) — app-owned sim, MOP renders
# -----------------------------------------------------------------------------
particles: $(PARTICLE_OUT)

$(PARTICLE_OUT): particle_demo.c $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(PLATFORM_LDFLAGS) -lm -lpthread -o $@
else
	@echo "ERROR: SDL3 not found. Run: nix develop"
	@exit 1
endif

run-particles: $(PARTICLE_OUT)
	./$<

# -----------------------------------------------------------------------------
# Water simulation (requires SDL3) — app-owned sim, MOP renders
# -----------------------------------------------------------------------------
water: $(WATER_OUT)

$(WATER_OUT): water_demo.c $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(PLATFORM_LDFLAGS) -lm -lpthread -o $@
else
	@echo "ERROR: SDL3 not found. Run: nix develop"
	@exit 1
endif

run-water: $(WATER_OUT)
	./$<

# -----------------------------------------------------------------------------
# Phase 1 showcase (requires SDL3) — multi-light, overlays, flex vertex, display
# -----------------------------------------------------------------------------
showcase: $(SHOWCASE_OUT)

$(SHOWCASE_OUT): showcase_demo.c $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(PLATFORM_LDFLAGS) -lm -o $@
else
	@echo "ERROR: SDL3 not found. Run: nix develop"
	@exit 1
endif

run-showcase: $(SHOWCASE_OUT)
	./$<

# -----------------------------------------------------------------------------
# Backend comparison (headless, no SDL needed)
# -----------------------------------------------------------------------------
compare: $(COMPARE_OUT)

$(COMPARE_OUT): compare_backends.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB) $(PLATFORM_LDFLAGS) -lm -lpthread -o $@

run-compare: $(COMPARE_OUT)
	./$<

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

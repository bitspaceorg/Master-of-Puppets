# Master of Puppets — Examples
# Builds example programs that link against libmop.a.
#
# Usage:
#   make              Build all examples
#   make headless     Build the headless PPM exporter
#   make interactive  Build the interactive SDL3 viewport
#   make run          Build and run the interactive viewport
#   make run-headless Build and run the headless PPM exporter
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
# Build directories and outputs
# -----------------------------------------------------------------------------
BUILD_DIR       := build
HEADLESS_OUT    := $(BUILD_DIR)/mop_headless
INTERACTIVE_OUT := $(BUILD_DIR)/mop_viewport

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all headless interactive clean run run-headless

all: headless
ifeq ($(HAS_SDL3),1)
all: interactive
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
# Interactive example (requires SDL3 via pkg-config)
# -----------------------------------------------------------------------------
interactive: $(INTERACTIVE_OUT)

$(INTERACTIVE_OUT): rotating_cube.c $(LIB) | $(BUILD_DIR)
ifeq ($(HAS_SDL3),1)
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) $< $(LIB) $(SDL3_LDFLAGS) $(PLATFORM_LDFLAGS) -o $@
else
	@echo "ERROR: SDL3 not found via pkg-config."
	@echo "       Enter the examples nix shell first:"
	@echo "         nix develop"
	@exit 1
endif

run: $(INTERACTIVE_OUT)
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

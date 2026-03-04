# Master of Puppets — Conformance Framework Build
#
# Builds the conformance runner binary that links against libmop.a.
#
# Usage:
#   make -C conformance              Build the runner
#   make -C conformance clean        Remove build artifacts
#
# From root:
#   make conformance                 Build library + runner
#   make conformance-tier1           Build + run tier 1
#   make conformance-tier2           Build + run tier 2

# Inherit toolchain from parent
CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wno-unused-parameter -Wno-missing-field-initializers \
            -fno-common \
            -I../include -I../src -I../third_party/stb -I.

ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -Og -g
endif

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DMOP_PLATFORM_MACOS=1
endif
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DMOP_PLATFORM_LINUX=1
endif

# Optional backends (passed through from root Makefile)
ifdef MOP_ENABLE_VULKAN
  CFLAGS += -DMOP_HAS_VULKAN=1
  VK_CFLAGS  := $(shell pkg-config --cflags vulkan 2>/dev/null)
  VK_LDFLAGS := $(shell pkg-config --libs vulkan 2>/dev/null)
  CFLAGS     += $(VK_CFLAGS)
  LDFLAGS    += $(if $(VK_LDFLAGS),$(VK_LDFLAGS),-lvulkan)
endif

ifdef MOP_ENABLE_OPENGL
  CFLAGS += -DMOP_HAS_OPENGL=1
  ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -framework OpenGL
  else
    LDFLAGS += -lGL
  endif
endif

# Lua support (auto-detect, same as root)
ifndef MOP_DISABLE_LUA
  LUA_CFLAGS  := $(shell pkg-config --cflags lua5.4 2>/dev/null || \
                          pkg-config --cflags luajit 2>/dev/null || \
                          pkg-config --cflags lua 2>/dev/null)
  LUA_LDFLAGS := $(shell pkg-config --libs lua5.4 2>/dev/null || \
                          pkg-config --libs luajit 2>/dev/null || \
                          pkg-config --libs lua 2>/dev/null)
  HAS_LUA     := $(if $(LUA_LDFLAGS),1,)
  ifeq ($(HAS_LUA),1)
    CFLAGS  += -DMOP_HAS_LUA=1 $(LUA_CFLAGS)
    LDFLAGS += $(LUA_LDFLAGS)
  endif
endif

# Paths
BUILD_DIR  := ../build
LIB_DIR    := $(BUILD_DIR)/lib
OBJ_DIR    := $(BUILD_DIR)/conformance
BIN_OUT    := $(BUILD_DIR)/conformance_runner

# Sources
SRCS := runner.c \
        scene_gen.c \
        camera_paths.c \
        metrics.c \
        validator.c \
        stability.c \
        report.c \
        perf_baseline.c

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

LDFLAGS += -L$(LIB_DIR) -lmop -lm -lpthread

# Default target
.PHONY: all clean

all: $(BIN_OUT)

$(BIN_OUT): $(OBJS) $(LIB_DIR)/libmop.a
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_OUT)

# Master of Puppets — Render Health Check Build
#
# Builds a single binary that exercises MOP's public API against a live
# viewport and asserts: no Vulkan validation errors, no sync hazards,
# no NaN pixels, CPU backend determinism, valid pick results.
#
# Usage:
#   make -C conformance              Build the runner
#   make -C conformance clean        Remove build artifacts
#
# From root:
#   make conformance                 Build library + runner
#   make conformance-run             Build and run

CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wno-unused-parameter -Wno-missing-field-initializers \
            -fno-common \
            -I../include -I../src

ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -Og -g
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DMOP_PLATFORM_MACOS=1
  LDFLAGS += -lc++
endif
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DMOP_PLATFORM_LINUX=1 -D_GNU_SOURCE
  LDFLAGS += -lstdc++
endif

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

BUILD_DIR  := ../build
LIB_DIR    := $(BUILD_DIR)/lib
OBJ_DIR    := $(BUILD_DIR)/conformance
BIN_OUT    := $(BUILD_DIR)/conformance_runner

LDFLAGS += -L$(LIB_DIR) -lmop -lm -lpthread

.PHONY: all clean

all: $(BIN_OUT)

$(BIN_OUT): $(OBJ_DIR)/runner.o $(LIB_DIR)/libmop.a
	$(CC) $(CFLAGS) $(OBJ_DIR)/runner.o $(LDFLAGS) -o $@

$(OBJ_DIR)/runner.o: runner.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_OUT)

# Master of Puppets — Examples
#
# Build examples against the MOP static library.
#
# Usage:
#   make                Build all examples
#   make snapshot       Build only snapshot
#   make clean          Remove build artifacts
#
# Assumes: ../build/lib/libmop.a exists (run `make` in parent first)

CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
            -fno-strict-aliasing -I../include -I../third_party/stb
LDFLAGS  := -L../build/lib -lmop -lm -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -lc++
else
  LDFLAGS += -lstdc++
endif

ifdef RELEASE
  CFLAGS += -O2 -DNDEBUG
else
  CFLAGS += -Og -g
endif

ifdef SANITIZE
ifeq ($(SANITIZE),asan)
  CFLAGS += -fsanitize=address -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address
endif
endif

# Optional: Vulkan backend (must match parent build)
ifdef MOP_ENABLE_VULKAN
  VK_CFLAGS  := $(shell pkg-config --cflags vulkan 2>/dev/null)
  VK_LDFLAGS := $(shell pkg-config --libs vulkan 2>/dev/null)
  CFLAGS     += $(VK_CFLAGS)
  LDFLAGS    += $(if $(VK_LDFLAGS),$(VK_LDFLAGS),-lvulkan)
endif

BUILD := ../build/examples
EXAMPLES := showcase

.PHONY: all clean $(EXAMPLES)

all: $(EXAMPLES)

$(BUILD):
	@mkdir -p $@

showcase: $(BUILD)/showcase

$(BUILD)/%: %.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)

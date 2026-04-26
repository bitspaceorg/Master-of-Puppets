# Master of Puppets — Tools Build
#
# Usage:
#   make              Build all tools
#   make clean        Remove build artifacts
#
# Tools link against libmop.a built by the parent Makefile.

CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wno-unused-parameter -Wno-missing-field-initializers \
            -I../include -I../src -I../third_party/stb
LDFLAGS  := -L../build/lib -lmop -lm

BUILD_DIR := ../build/tools

.PHONY: all clean

all: $(BUILD_DIR)/mop_convert $(BUILD_DIR)/mop_font_bake

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/mop_convert: mop_convert.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

# mop_font_bake compiles stb_truetype's implementation directly into
# the binary; libmop is not required so we link only -lm.  The relaxed
# warning flags are scoped to this target because stb_truetype's
# upstream code triggers -Wpedantic and -Wsign-compare under -Werror.
$(BUILD_DIR)/mop_font_bake: mop_font_bake.c | $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wno-unused-parameter \
	      -Wno-unused-but-set-variable -Wno-unused-function \
	      -I../include -I../src -I../third_party/stb \
	      $< -lm -o $@

clean:
	rm -rf $(BUILD_DIR)

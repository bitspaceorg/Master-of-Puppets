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
            -I../include -I../src
LDFLAGS  := -L../build/lib -lmop -lm

BUILD_DIR := ../build/tools

.PHONY: all clean

all: $(BUILD_DIR)/mop_convert

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/mop_convert: mop_convert.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR)

# FlightBench device models: plain C, no emulator dependency.
#   make            -> build/libfbdevices.a
#   make test       -> build and run host-side unit tests
#   make tools      -> build/fb_manifest (device catalog JSON for the core manifest),
#                      build/fb_session (resolve a bench as -M session-check=on does)
CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wpedantic
CFLAGS  += -Iinclude
ifneq (,$(findstring MINGW,$(shell uname -s)))   # MSYS2/mingw (OS= is not exported there)
CFLAGS  += -D__USE_MINGW_ANSI_STDIO=1   # mingw: C99 printf (%zu) instead of msvcrt's
endif
BUILD   := build

SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/*.c))

all: $(BUILD)/libfbdevices.a

tools: $(BUILD)/fb_manifest $(BUILD)/fb_session

$(BUILD)/fb_manifest $(BUILD)/fb_session: $(BUILD)/fb_%: tools/fb_%.c $(BUILD)/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) $< -L$(BUILD) -lfbdevices -lm -o $@

$(BUILD)/libfbdevices.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(BUILD)/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) $< -L$(BUILD) -lfbdevices -lm -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all test tools clean

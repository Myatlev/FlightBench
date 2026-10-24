# FlightBench flight dynamics models: plain C, no emulator dependency.
#   make            -> build/libfbfdm.a
#   make test       -> build and run host-side tests
CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wpedantic
CFLAGS  += -Iinclude -I../devices/include -D_USE_MATH_DEFINES -D_DEFAULT_SOURCE
BUILD   := build
# From the device models: the JSON reader (scenario parsing; the core links both libraries) and,
# for the tests, the ESC + DShot models the core drives the motor with.
DEVSRC  := ../devices/src/json.c ../devices/src/esc.c ../devices/src/dshot.c

SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/*.c))

all: $(BUILD)/libfbfdm.a

$(BUILD)/libfbfdm.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(BUILD)/libfbfdm.a $(DEVSRC) | $(BUILD)
	$(CC) $(CFLAGS) $< $(DEVSRC) -L$(BUILD) -lfbfdm -lm -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all test clean

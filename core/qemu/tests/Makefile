# Host unit tests of the QEMU machine's emulator-independent parts (no QEMU needed):
#   make -C core/qemu/tests test   -> test_i2c (hw/arm/stm32_i2c.c.inc, with devices/' DPS310)
#                                     test_i2c_v1 (hw/arm/stm32_i2c_v1.c.inc under iNav's F4 I2C
#                                     driver, with devices/' SPL06 and QMC5883L)
#                                     test_prof (hw/arm/fb_prof.c.inc, the prof=on ledger)
#                                     test_session (devices/src/session.c, the bench resolver of
#                                     -M session=; the test_wiring.py cases and the C-only parts)
#                                     test_msp (hw/arm/fb_msp_modes.c.inc, the `modes` decoders, on
#                                     MSP replies captured from RotorFlight and iNav: fixtures/)
#                                     test_bench_state (hw/arm/fb_bench_state.c.inc, the bench
#                                     state's restore verdict and the script's delta)
#                                     test_outputs (hw/arm/fb_outputs.c.inc, whether a timer
#                                     output reaches its pad: GPIO AF, CCxE, BDTR.MOE)
#   make -C core/qemu/tests equiv  -> test_session_equiv.sh: -M session-check=on of QEMU=<binary>
#                                     (default run.sh's) vs the Python reference, every bench
#   make -C core/qemu/tests equiv-host -> the same through devices/build/fb_session (no QEMU)
#   make -C core/qemu/tests fuzz-msp -> test_msp_fuzz, ASan+UBSan: every fb_msp_modes.c.inc
#                                     decoder against truncated/oversized/random MSP payloads
CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wpedantic
DEV     := ../../../devices
CFLAGS  += -I$(DEV)/include
BUILD   := build

REPO    := $(abspath ../../..)
# MSYS2: the test is a native exe, it needs C:/... rather than /c/...
REPO    := $(shell cygpath -m '$(REPO)' 2>/dev/null || echo '$(REPO)')
QEMU    ?= $(HOME)/flightbench-spike/qemu/build/qemu-system-arm

all: $(BUILD)/test_i2c $(BUILD)/test_i2c_v1 $(BUILD)/test_prof $(BUILD)/test_session $(BUILD)/test_msp \
     $(BUILD)/test_bench_state $(BUILD)/test_outputs

$(DEV)/build/libfbdevices.a: FORCE
	$(MAKE) -C $(DEV)

$(BUILD)/test_i2c: test_i2c.c ../hw/arm/stm32_i2c.c.inc $(DEV)/build/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) $< -L$(DEV)/build -lfbdevices -lm -o $@

$(BUILD)/test_i2c_v1: test_i2c_v1.c ../hw/arm/stm32_i2c.c.inc ../hw/arm/stm32_i2c_v1.c.inc $(DEV)/build/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) $< -L$(DEV)/build -lfbdevices -lm -o $@

$(BUILD)/test_prof: test_prof.c ../hw/arm/fb_prof.c.inc | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_session: test_session.c $(DEV)/build/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) -DREPO_ROOT='"$(REPO)"' $< -L$(DEV)/build -lfbdevices -lm -o $@

$(BUILD)/test_msp: test_msp.c ../hw/arm/fb_msp_modes.c.inc $(DEV)/build/libfbdevices.a | $(BUILD)
	$(CC) $(CFLAGS) -DFIXTURES='"$(REPO)/core/qemu/tests/fixtures"' $< -L$(DEV)/build -lfbdevices -lm -o $@

$(BUILD)/test_bench_state: test_bench_state.c ../hw/arm/fb_bench_state.c.inc | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_outputs: test_outputs.c ../hw/arm/fb_outputs.c.inc | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_msp_fuzz: test_msp_fuzz.c ../hw/arm/fb_msp_modes.c.inc | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_i2c $(BUILD)/test_i2c_v1 $(BUILD)/test_prof $(BUILD)/test_session $(BUILD)/test_msp \
      $(BUILD)/test_bench_state $(BUILD)/test_outputs
	$(BUILD)/test_i2c
	$(BUILD)/test_outputs
	$(BUILD)/test_i2c_v1
	$(BUILD)/test_msp
	$(BUILD)/test_bench_state
	$(BUILD)/test_prof $(BUILD)/prof.json
	python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); assert d["type"] == "prof" and d["accounts"] and "wake_ns" in d["handoffs"]; print("test_prof: JSON OK,", len(d["accounts"]), "accounts")' $(BUILD)/prof.json
	rm -rf $(BUILD)/session-tmp && mkdir -p $(BUILD)/session-tmp/ac $(BUILD)/session-tmp/home "$(BUILD)/session-tmp/a,b"
	$(BUILD)/test_session

fuzz-msp: $(BUILD)/test_msp_fuzz
	$(BUILD)/test_msp_fuzz

equiv:
	QEMU=$(QEMU) ./test_session_equiv.sh

equiv-host:
	$(MAKE) -C $(DEV) tools
	SESSION_TOOL=$(DEV)/build/fb_session ./test_session_equiv.sh

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all test fuzz-msp equiv equiv-host clean FORCE

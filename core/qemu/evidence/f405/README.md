# speedybee-f405-wing: stock iNav 9.1.0 on the STM32F405 machine (phase 1), 2026-09-29

> **2026-10-24:** the flight models were rewritten from first principles (`docs/fdm.md`); the flights and numbers
> below were made with the earlier models. The current baseline hashes are in `docs/contributing.md` (gates table).

Branch `f405-machine`, homelab (i7-4790), QEMU 11.1.1 + this tree in `~/flightbench-spike/qemu-prof`
(built from the working tree that became `acaea04`; the build id says `3a3e445+dirty` because it was
built before those commits). Firmware: the release asset `inav_9.1.0_SPEEDYBEEF405WING.hex`, unmodified
(fw hash `3bfe4bef168e`). Bench: `core/qemu/sessions/f405-boot.json` (no wiring: onboard devices only,
icount shift 2) with the ports moved to 31280/31281 (HMP 31282, motion 31283, pilot 31284).
Symbols for the analysis below come from an ELF built from the 9.1.0 tag
(`~/flightbench-spike/inav/build-f405/bin/SPEEDYBEEF405WING.elf`); its binary differs from the release
hex in 17 bytes (build date/time strings), same layout.

| file | what |
|---|---|
| `f405-2026-09-29.log` | `scripts/f405_evidence.py` stages A–E on the committed code |
| `determinism-exact-stop-2026-09-29.log` | stage E, an earlier run of the same code where the step stops matched (below) |
| `rs7-regression-2026-09-29.log` | `scripts/speed_evidence.sh "P S E"` on the RS7 liftoff bench, the nexus-xr machine from the same build + prof account count |
| `tests-2026-09-29.log` | every test suite |

## Results

| check | result |
|---|---|
| boot | the stock hex boots; the control server's MSP probe answers at t = 7.94 s: `firmware says: inav 9.1.0 SPEEDYBEEF405WING` |
| config script | blank sector 11 → the core enters the CLI on USART6 at t = 7.94, `config: feature -GPS`, `save` at 8.06; the firmware writes sector 11 and reboots; at 9.13 the sector (0x080E0000, 128 KiB) becomes the snapshot for fw `3bfe4bef168e` + script `84b258b78914` |
| CLI over USART6 | `version`: `INAV/SPEEDYBEEF405WING 9.1.0 Jul 8 2026 / 02:13:52 (15317503)`; `status`, `tasks`, `diff` answer (full text in the log) |
| `diff` after the reboot | `set acc_hardware = ICM42605`, `set mag_hardware = NONE`, `set baro_hardware = NONE` (the autodetect results iNav saves), `set align_mag = CW270FLIP`, `set blackbox_rate_denom = 2`, the target's blackbox field list; `feature -GPS` does not show because GPS is already off in the target's defaults (the line is a guard, `boards/speedybee-f405-wing/defaults.cli`) |
| sensors | `CPU Clock=168MHz, GYRO=ICM42605, ACC=ICM42605`; clocks SYSCLK/HCLK 168, PCLK1 42, PCLK2 84 MHz; `Sensor status: GYRO=OK, ACC=OK, MAG=NONE, BARO=NONE, RANGEFINDER=NONE, OPFLOW=NONE, PITOT=NONE, GPS=NONE`; `I2C Errors: 0` (the NACKing I2C1 does not count as errors, no hang); `OSD: MAX7456 [30 x 16]`; `SD card: Startup failed` |
| load | `System load: 12, cycle time: 517, PID rate: 1934, RX rate: 49, System rate: 9`; `tasks`: GYRO 3952 Hz, PID 1984 Hz, total 18.8 % max / 13.3 % avg |
| battery | ADC reads 0: `Voltage: 0.00V (0S battery - NOT PRESENT)`; no arming flag for it |
| arming flags, CLI `status` | `CAL ACC RX CLI` (CLI only because we are in the CLI) |
| arming flags, control server (`MSP2_INAV_STATUS`) | t = 25.8 s: `armed: false`, `mode: FAILSAFE`, `arming_flags: [CAL, ACC, RX]`, `cpu_pct: 2` |
| snapshot | `firmware` with the same hex (whole flash erased) at t = 25.8: `config: blank config sector: the snapshot for fw 3bfe4bef168e script 84b258b78914 written back (the script is not run again)`; `diff` afterwards is the same |
| speed, SysTick idle skip off | 49.98, 49.99 host s per 10 virtual s (0.200× realtime) |
| speed, SysTick idle skip on | 40.36, 39.89 host s per 10 virtual s (0.25×); ~162 k skips, ~6.0 virtual s skipped by t ≈ 21 s. **1.25× faster**, not the RotorFlight gain: the rest is the firmware's busy work (GYRO 4 kHz polled over SPI, PID 2 kHz) |
| determinism | below |
| RS7 regression (nexus-xr) | FDM log `c5a2b9bb6c49`, 2251 rows, 3 of 3 runs (P + 2 × S); 9.70 / 9.66 host s per 10 virtual s; 67 prof accounts (as `main`) |
| tests | `make -C devices test` (incl. `test_max7456`), `make -C fdm test`, `make -C core/qemu/tests test` (test_i2c 68, test_prof 3851, test_session 172), `equiv` 180/180 through the QEMU build, `equiv-host` 180/180, `test_wiring.py` 55 OK, py_compile OK |

### Arming blockers with no aircraft yet
- `RX`: no receiver (the bench wires none; a `crsf` on uart1 would feed it).
- `ACC`: accelerometer not calibrated. iNav needs its 6-point calibration (the configurator's
  `calibrate acc` or `accZero`/`accGain` values in the script); the model's accel is exact 1 g at
  rest, but iNav does not assume that.
- `CAL`: "sensors calibrating". `areSensorsCalibrating()` (fc/fc_core.c:145) is true while the
  gravity calibration of the position estimator or the acc calibration is incomplete and ACC is
  required, or the gyro calibration is running. It stays set in these runs; which of the three
  holds it was not isolated (most likely it follows `ACC`: the gravity calibration uses the
  calibrated accelerometer). To check with the acc calibration in the script.
- The control server's `mode` is `FAILSAFE` (no RX). Not in the list: battery (not present, no
  flag), `HWFAIL` (with `feature -GPS` no GPS timeouts), `NAV`, `ANGLE` (the board is level).

### Determinism
`f405_evidence.py` stage E pauses each run (a host-timed `pause`), reads the virtual clock in ns
(`vt_ns=` of the qom `bench` line) and sends one `step` that ends at exactly 20 000 000 000 ns, then
dumps SRAM, CCM and the config sector (HMP `pmemsave`).

- The step does **not** stop the vCPU at an exact instruction: `ctl_step_done` only requests a VM
  stop and the vCPU keeps running until the main loop handles it, so the stop lands 3–9 µs after the
  deadline and varies between runs (this run: 20 000 005 428 / …5 468 ns off, …6 604 / …3 136 ns on).
  Between different stop points the RAM differs, as it must.
- When the stop points match, so does the memory. In `determinism-exact-stop-2026-09-29.log` both
  skip-off runs stopped at 20 000 008 552 ns: SRAM, CCM and config sector identical. The skip-on runs
  stopped 40 ns (10 instructions) apart: SRAM identical, CCM differs in 8 bytes, all in the main
  stack (0x1000E800–0x10010000, no variable there by the symbols of the 9.1.0 ELF).
- The idle skip makes the same decisions in both runs: 140 826 skips, 5.233 virtual s skipped, at
  the stop in every skip-on run (these do not change within a few µs).
- The config sector is identical in every run (`17107d9f5aaf`).

## Caveats / not verified
- MAX7456: **not in the brief**. It was added because the firmware does not boot without it (see
  `boards/speedybee-f405-wing/README.md`); a register-level model of 222 lines + a test, no video.
  The resolver now lists it as modelled on `osd1` (onboard, not detachable).
- The step stop jitter above is a property of the control server's `time step` (shared with
  nexus-xr); its comment says the vCPU stops at the deadline rounded to an instruction, which holds
  for the timer callback but not for where the VM actually stops. Not changed here.
- `status` shows `Current Time: 2041-06-28T01:04:00` (the RTC stub); not looked into.
- The snapshot is in memory only (per QEMU process); a new core runs the script again (~1 virtual s).
- Not exercised here: outputs (S1 ONESHOT125 pulse and servos S1..S11 at 50 Hz: the machine models
  them, but this bench wires no ESC/servos and there is no `platform_type`/mixer script yet); a CRSF
  receiver on uart1 (`f405-crsf.json` bench exists, not run for this evidence); a rejected CLI line
  (`config: error`) on the real firmware; the power-cycle path of the snapshot (MCU reset and
  `firmware` reload were exercised).
- Speed is low (0.2×/0.25× realtime at icount shift 2): the iNav target runs GYRO at 4 kHz polled
  and PID at 2 kHz with no EXTI; not profiled yet (`PROF=1` works on this machine).

## Review, 2026-09-29 (`review-2026-09-29.log`)
- **RS7 gate**: main (32685d9) and this branch built in the same tree, alternating runs: FDM log
  `c5a2b9bb6c49` in 3/3 runs each, byte-identical; 67 prof accounts both, the same per-event counts;
  speed main 10.15 / 9.93, branch 9.82 / 9.74 host s per 10 virtual s (a loaded host); with the
  review fixes 9.66 / 9.62, same hash.
- **SoC split**: `info mtree` (flat and tree), `info qtree`, `info qom-tree` of `nexus-xr` on the RS7
  bench, main vs branch: identical once the address-space print order is sorted; NVIC `num-irq`,
  priority bits, `info irq` identical.
- **Idle-skip determinism**: the stop jitter above is only the host-timed `pause`. Stopped instead at
  a guest-defined point (gdbstub hardware breakpoint on `taskSystem`, the 200th / 330th hit), two
  skip-on runs are identical in every respect: virtual clock (32 061 499 400 / 45 063 688 573 ns),
  idle skips (403 784 / 691 176), registers, SRAM, CCM and the config sector.
- **equiv** 180/180 ×3, **equiv-host** 180/180 ×3: the 179/180 flake did not come back.
- **Bare-boot profile** (`f405-bare-boot-skip-{off,on}-…`, prof=on, 20 virtual s, no plan): 81–82 %
  of the host time is guest execution (TCG); of the accounted part, `armv7m.nvic` (≈4.8 M reads per
  virtual s, 147 ns each, 10 % of wall) and `armv7m.systick` (≈4.8 M/s, 5–7 %): iNav's `micros()`
  (SysTick VAL + SCB ICSR) polled by the scheduler; then DWT (2.5 M/s, 1 %). SPI1 (gyro, 67 k/s),
  SPI2 (OSD, 45 k/s), GPIO (CS, 144 k/s), USART6 and the USB OTG stub (60 k/s during the boot's
  bounded GRSTCTL loops) are each ≤ 0.25 %. With the skip on the NVIC/SysTick reads drop only ~7 %:
  the skip catches a fraction of the idle polling.
- **main does not build through `apply.sh`** (not this branch): `devices/src/baro_common.h` is not
  copied into `hw/arm/fb-devices/` (only `src/*.c`), and `fdm/include/fb/plane_scenario.h` includes
  `"fb/plane_fdm.h"`, which does not resolve from inside `fb-devices/fb/`. The main build above used
  a copy of the header and a `fb-devices/fb/fb -> .` symlink in the QEMU tree; the merge inherits both.

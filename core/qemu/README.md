# core/qemu — QEMU backend: `nexus-xr` (STM32F722) and `speedybee-f405-wing` (STM32F405) machines

A QEMU machine that boots the **unmodified** RotorFlight `STM32F7X2_OSD` build (RadioMaster
Nexus-XR) to its scheduler, answers on the CLI/MSP over USART1, **runs the gyro** (an emulated
ICM-42688-P on SPI1 from `devices/`, sampled at the rate the firmware programs) and **flies the RS7**:
servo pulses and DShot600 frames are read from the timers and their DMA, a CRSF receiver on UART5
carries a scripted pilot, the ESC answers with bidirectional-DShot eRPM, the onboard baro (a DPS310/
SPL06 model on I2C3) reports the FDM's altitude, and the helicopter FDM from `fdm/` closes the loop
through the IMU, all in lockstep with QEMU's virtual clock. Upstream QEMU has
no STM32F7 machine; this directory holds one as a single source file applied, together with the
device and FDM libraries, to a QEMU v11.1.1 tree.

## Files
| file | what |
|---|---|
| `hw/arm/stm32_soc.c.inc` | the SoC and board descriptions both machines are built from ("Machines" below): `FbSoc` (CPU, clocks incl. per-bus timer clocks, flash + sector table, RAM regions, UID/flash-size addresses, NUM_IRQ, peripheral instances with base + IRQ + model variant) and `FbBoard` (machine type, IMU/OSD chip selects, outputs, receiver USARTs, MSP UART, blob or CLI configuration, config sector, idle-skip trigger); included by the machine |
| `hw/arm/nexus_xr_ctl.c.inc` | the control server of `docs/protocol.md` (TCP 31250), included by the machine: clients, JSON, MSP master + CLI on USART1, time control, reset/firmware/power, signals, manifest, fault injection |
| `hw/arm/stm32_i2c.c.inc` | the STM32F7 I2C (v2) master: registers + bus state machine, emulator-independent (hooks for the timer, IRQ lines, log); included by the machine, host-tested by `tests/test_i2c.c` |
| `hw/arm/fb_fdm.c.inc` | the flight model in the machine (any type, `fdm/include/fb/fdm.h`): the aircraft file's `fdm_params` handed to the model, the physics tick (servos by actuator name → `fb_fdm_step`, the ESC's rpm, the shaft load back, the board's IMU alignment), events, the plane state/signals, `launch`/`constraint`/`env`/scenarios; included by the machine |
| `hw/arm/fb_msp_modes.c.inc` | decoders of the firmware's mode messages (MSP_BOXNAMES/BOXIDS/RX_MAP/MODE_RANGES/_EXTRA; iNav and RotorFlight layouts), emulator-independent; included by the control server, host-tested by `tests/test_msp.c` |
| `hw/arm/fb_bench_state.c.inc` | the bench state's decisions (`-M bench-state=`): the metadata's file name, whether a sector + metadata are restored, the script's delta; emulator-independent, included by the control server, host-tested by `tests/test_bench_state.c` |
| `hw/arm/fb_outputs.c.inc` | whether a timer output reaches its pad (GPIO alternate function + AF number, CCxE/CCxNE, BDTR.MOE on TIM1/TIM8), from register values, emulator-independent (`tests/test_outputs.c`) |
| `hw/arm/fb_prof.c.inc` | the host-time ledger of `-M prof=on` (accounts, nesting, latency histograms, the `prof` JSON), emulator-independent; included by the machine, host-tested by `tests/test_prof.c` |
| `patches/` | changes to QEMU itself, applied by `apply.sh` (`patches/README.md`): `prof-qemu.diff`, observe-only hooks for the ledger (vCPU slices, kicks, main-loop poll, BQL contention) |
| `tests/` | host unit tests of the machine's emulator-independent parts, no QEMU: `make -C core/qemu/tests test` (`test_i2c`: the I2C controller with a scripted memory slave and the DPS310 model; `test_prof`: the ledger; `test_session`: the bench resolver, `devices/src/session.c`; `test_msp`: the `modes` decoders on captured MSP replies, `tests/fixtures/`; `test_bench_state`: the bench state's restore verdict and delta; `test_outputs`: the output gate on the register values iNav leaves with and without `PWM_OUTPUT_ENABLE`); `make -C core/qemu/tests equiv QEMU=<binary>`: `test_session_equiv.sh`, the machine's `session-check` against the Python reference on every bench (`session_ref.py`, `gen_benches.py`, `benches/`) |
| `hw/arm/nexus_xr.c` | the machine: memory map, writable flash + FLASH controller, DWT CYCCNT, DBGMCU, USARTs, RCC/PWR/ADC register behaviour, GPIO A..D, SYSCFG/EXTI, SPI1 + DMA2, the IMU wiring and motion source, I2C3 + the baro, I2C1 + SPL06 + compass (F405), the GPS wire, ADC1 + the battery (F405), TIM1/2/3, DShot + ESC, CRSF receiver + pilot socket, FDM tick, permissive stubs |
| `hw/arm/stm32_i2c_v1.c.inc` | the STM32F4 I2C (v1) master, emulator-independent (`tests/test_i2c_v1.c`) |
| `apply.sh <qemu-src>` | applies `patches/*.diff` (skipped when already applied), writes `hw/arm/fb_build.h` (build id), copies the machine (`nexus_xr.c`, `nexus_xr_ctl.c.inc`, `stm32_i2c.c.inc`, `stm32_i2c_v1.c.inc`, ...), `devices/` and `fdm/` sources + headers (as `hw/arm/fb-devices/`, public headers in `fb-devices/fb/`) into a QEMU tree, adds `CONFIG_NEXUS_XR` to `hw/arm/Kconfig` and all those sources to `hw/arm/meson.build` with `fb-devices/` on the include path (idempotent) |
| `build.sh [qemu-src]` | clone QEMU v11.1.1 (if missing), apply, configure `arm-softmmu`, build `qemu-system-arm` |
| `run.sh --manifest [--pretty]` | print the core manifest (boards with pads/slots, devices with ports/props/faults, aircraft) and exit: `-M session-check=on` of `$QEMU` (no bench), or `scripts/manifest.py` when there is no QEMU binary or `BOARD_CONFIG` is set |
| `run.sh --session <json>` | the headless launcher of `docs/protocol.md` "Launching the core": `-M nexus-xr,session=<json>,root=<repo>` (the machine resolves the bench), `-icount` and the control/MSP ports from the `launch:` line of `session-check=on`, the sockets; no python, no temp files |
| `run.sh <elf> [qemu args]` | bare boot without a bench: firmware (`fw=`), board config (+ `PROFILE=` profiles) as the custom-defaults blob (`defaults=`, `mkdefaults.sh`), manifest from `manifest.py`, the machine's default devices |
| (both) | control server on TCP :31250, USART1 on TCP :31235, HMP monitor on TCP :31236, IMU motion source on TCP :31237, pilot on TCP :31238, `MACHINE_OPTS=` machine options (they win over the bench's), `PROF=1` the host-time ledger (`PROF_PERIOD=<s>` periodic `prof` messages), `GDB=1` for a halted gdbstub on :31234 |
| `scripts/wiring.py` | **reference implementation** (the machine runs its C port, `devices/src/session.c`): the wiring model (below): board pads from the unified config + `boards/<id>/board.json`, the device catalog, `MODELLED` (what the machine has behind which pad), `resolve()` of a session's wiring, `machine_options()` |
| `scripts/manifest.py [--board-config f] [--pretty] [--out f]` | **reference implementation** (the machine builds the same bytes itself): the manifest from `wiring.py`: board `pads` (+ the old `slots`), device descriptors with `ports` (`devices/build/fb_manifest`, built if missing), `implemented` from `MODELLED`, `models/*/aircraft.json`; bare `run.sh <elf>` still writes it to its tmp dir and passes `manifest=` |
| `scripts/test_wiring.py` | unit tests of the wiring model: the pad table vs the 13 slots the app derives, old/new session forms, every rejection |
| `scripts/wiring_evidence.sh [stages]` | wiring model evidence: manifest, session validation, flight equivalence + determinism, reversed servo into the FDM, sessions without receiver/ESC, ctl/fault evidence (`evidence/wiring-2026-09-26.log`) |
| `scripts/fault_evidence.py` | manifest + fault injection evidence: manifest checks, then a flight with each fault in turn and the firmware's reaction |
| `scripts/session.py <json> <tmpdir>`, `session.py --check <json>` | **reference implementation** of `-M session=` (not on the launch path any more): session description → the former run.sh variables (firmware, defaults, machine options, icount, ports); the wiring is resolved by `wiring.py` (errors reject the session, unmodelled entries are reported and left out); `--check` prints the resolved assembly |
| `sessions/trainer.json` | the trainer plane on the speedybee-f405-wing: stock iNav 9.1.0 (`firmware/speedybee-f405-wing/`), CRSF uart1, ESC S1, servos S3..S6 = aileron_l/r, elevator, rudder, the IMU with a 20 mg / 0.5 % accel error (`models/trainer/README.md`) |
| `sessions/trainer-osd.json` | the trainer + an HD VTX OSD (`msp_osd`, MSP DisplayPort) on uart4, as the app's `inav-trainer-fpv-osd` preset |
| `scripts/inav_osd_evidence.py [stages]` | iNav's MSP DisplayPort OSD evidence (`evidence/inav-osd/`): the OSD bench's first start, boot with the stream recorded (`-M osd-log=`), rate/font/text art/CLI, plane-launch twice (FDM md5), prof=on; ports 31280–31284 |
| `scripts/modes_evidence.py [stages]` | modes / diff / functions evidence (`evidence/modes/`): the RS7 (`modes`, `state.fc.boxes` armed, `diff` on request, calibrate_gyro, calibrate_acc level, errors) and the trainer (the first start with its diff, a late client's replay, launch, calibrate_gyro, calibrate_acc standalone, a `diff` request); MSP fixtures for `tests/test_msp.c`; ports 31280–31284 |
| `scripts/config_layers_evidence.py [stages]` | the config layers evidence (`evidence/config-layers/`): the trainer with bench lines and a disabled device line — session-check provenance and the conflict, first start (`sent`), `diff against:"script"`, a replayed Modes save, `config apply` (delta, nothing, full), `config snapshot` → `bench_cli` → restart / reset first_start, `config export`, refusals; ports 31270–31274 (`PORT_BASE`) |
| `scripts/cli_norm.py` | the reference of `devices/src/cli_norm.c` (CLI lines as settings; `--diff <script> <diff.txt>`), used by `session.py` for the conflicts and by `test_session_equiv.sh` |
| `scripts/bench_state_evidence.py [stages]` | the bench state evidence (`evidence/bench-state/`): first start, a replayed Modes save, the CLI indicator + exit, restart, the GPS wired (the delta), `config` reset, damaged state files; ports 31270–31274 (`PORT_BASE`) |
| `scripts/msp_capture.py <msp port> <out.json> [cmd ...]` | raw MSP v2 replies from a running core's MSP socket as JSON fixtures (`tests/fixtures/`) |
| `scripts/plane_evidence.py [stages]` | phase-3 evidence (`evidence/plane/`): the first start with the ACC calibration, `plane-launch` twice (determinism) + report, `ground-takeoff` with the link lost, the flight profile; ports 31280–31284 |
| `sessions/rs7.json`, `sessions/rs7-pads.json`, `sessions/rs7-osd.json` | the RS7 session as the current app sends it (old form, slots) and the same bench in the new form (device ports on pads, by label where the label is known) |
| `scripts/ctl_evidence.py` | step-C evidence: drives every protocol stage against a running core, one line per check |
| `scripts/ground_evidence.py` | ground contact evidence: gentle landing keeps the heading, `crash` + upright the firmware follows, `reset aircraft` as a rotation the IMU sees |
| `scripts/mkdefaults.sh` | builds the custom-defaults blob (`## Rotorflight Custom Defaults` + board config + profiles) at 0x08002808, as the configurator does when flashing; used by bare `run.sh <elf>`, **reference implementation** of the blob `-M session=` builds in memory |
| `scripts/flight.py` | scripted pilot: a stick schedule in virtual time on the pilot socket, MSP_STATUS/ATTITUDE/DEBUG polling (`--csv`), compact log lines; plans `liftoff`/`spool` (RS7) and `plane-launch`/`ground-takeoff` (trainer, `--t0`: channels, `launch`, link loss, all with `at`) |
| `scripts/flight_report.py <fdm.csv> <msp.csv> [merged.csv]`, `--plane <fdm.csv>` | joins the machine's FDM log with the MSP samples, prints lift-off/attitude/head-speed numbers; `--plane`: the plane FDM log alone (it carries the core's MSP view): arming/mode/blocker changes, launch timing, climb, level flight, failsafe, a table |
| `scripts/outputs_evidence.sh <elf>`, `scripts/flight_evidence.sh <elf>` | the milestone-2 step-B evidence runs (outputs; arming + governor + lift-off) |
| `scripts/cli.py <port> [wait] [cmd...]` | sends `#`, then each command (default `status`), and prints the replies (`CLI_DRAIN=s` per command) |
| `scripts/configurator_handshake.py <address> [inav\|rotorflight] [--hex] [--timeout=s] [--tabs]` | a configurator's TCP connect replayed: the address parsed as the configurator parses it (iNav splits `host:port` on ':', so `tcp://…` fails to open; RotorFlight takes only `tcp://host[:port]`), then its request chain (iNav: MSP v2 API_VERSION, FC_VARIANT, FC_VERSION, NAME, BOXIDS, DATAFLASH_SUMMARY; RotorFlight: MSP v1 API_VERSION, FC_VARIANT, FC_VERSION, BUILD_INFO, BOARD_INFO, NAME), 3 s retry, 10 s window; exit 0 = connected; `--tabs` (iNav): then the configurator's post-connect and tab-load requests and what it decides from them (the "Default values" dialog from `applied_defaults`, the Status tab's "Mixer not configured" from the mixer's `appliedMixerPreset`, the Mixer tab's rules, the Outputs tab's servo rates and `PWM_OUTPUT_ENABLE`), exit 3 when any of them says "not configured" |
| `scripts/msp.py <port> raw_imu [n]` / `acc_calibration` | MSP v1: `MSP_RAW_IMU` (acc 512 = 1 g, gyro raw LSB, 16.384 = 1 deg/s) / `MSP_ACC_CALIBRATION` |
| `scripts/hmp.py <port> cmd...` | HMP monitor commands, e.g. `"qom-get /machine imu-samples"` |
| `scripts/gdb-boot.gdb` | breakpoints for the boot sequence check (`main`, `init`, `taskSystemLoad`, `failureMode`, `HardFault_Handler`) |
| `scripts/f405_evidence.py <bench> [stages]` | speedybee-f405-wing evidence (`evidence/f405/`): boot + the CLI script + `save`, CLI over USART6, the iNav status decoder, the config snapshot, speed with/without the SysTick idle skip, determinism (paused at t = 20 s: RAM hashes); ports 31280–31284 |
| `scripts/speed_inav_evidence.py "<stages>" [label]` | speed of the iNav machine (`evidence/speed-inav/`): first start, bare boot, boot from the snapshot, the trainer's plane-launch flight (host s per 10 virtual s + FDM md5), the flight.py gate, reboot after the CLI `exit`, prof=on profiles; refuses to start while one of its ports (31280–31284) is in use |
| `scripts/speed.py` | host seconds per N virtual seconds (polls `sysTickUptime` through HMP `xp`); `--session S [--machine-opts ...]` runs the whole bench through `run.sh --session` and reports the idle-skip counters |
| `scripts/idle_skip_evidence.sh <session> [stages]` | idle skip off vs on: speed idle/in flight, CLI `tasks`/`status`, flight equivalence + determinism, ground contact, realtime, USART1 MSP (`evidence/idle-skip-2026-09-26.log`) |
| `scripts/realtime_evidence.py [ctl] [pilot]` | realtime throttle at several speeds (measured vs `realtime_factor`), ping round trip while throttled, a pilot client that never reads |
| `scripts/baro_evidence.sh [stages]`, `scripts/baro_evidence.py` | baro evidence: boot/status/tasks, pad altitude, lift-off base vs this build + altitude tracking table, detached, `dps310.freeze` in a climb and across an MCU reset / power cycle, speed + two-run determinism of the FDM log and of the baro trace, ctl/fault evidence (`evidence/baro-2026-09-26.log`) |
| `scripts/uart_evidence.py [ctl] [msp]` | USART1 with two masters: `rc` → `fc.rc_us` latency with a quiet socket, after host frames carrying 0x23, while a host streams MSP requests |
| `scripts/osd_evidence.sh [stages]`, `scripts/osd_evidence.py` | MSP DisplayPort OSD evidence: tests + manifest, boot with the device (rate, full/delta, text art, CLI, canvas), lift-off with the OSD, reset/fault/subscribe, without the device (flight unchanged vs base), speed, ctl/fault evidence (`evidence/osd-2026-09-26.log`) |
| `scripts/prof.py run\|show\|diff` | host-time profile of a running `PROF=1` core: record (optionally flying flight.py's plan) to a JSON file, print it, compare two (`evidence/prof/README.md`) |
| `scripts/prof_evidence.sh [session] [stages]` | the prof evidence: tests, RS7 liftoff and bare-boot profiles, overhead prof=off vs on, FDM-log determinism (`evidence/prof/`) |
| `evidence/` | logs from the acceptance runs |

## Machines

One machine source (`hw/arm/nexus_xr.c`) with the peripheral models; the chip and the board are data
(`hw/arm/stm32_soc.c.inc`), one QEMU machine type per board. A bench names its board, the board its
machine (`board.json` `machine`); `session-check` prints it in the `launch:` line (`machine=`), and
`run.sh --session` passes `-M <machine>`. A bench for another board's machine is rejected (exit 2).

| machine | board / SoC | firmware | configuration | MSP/CLI UART | idle-skip trigger |
|---|---|---|---|---|---|
| `nexus-xr` | RadioMaster Nexus-XR / STM32F722 (216 MHz, cortex-m7) | RotorFlight `STM32F7X2_OSD` (`OPTIONS=USE_MSP_UART`) | custom-defaults blob at 0x08002808 (`defaults=`, or in memory from the bench) | USART1 | DWT CYCCNT reads |
| `speedybee-f405-wing` | SpeedyBee F405 WING / STM32F405 (168 MHz, cortex-m4) | stock iNav 9.1.0 `SPEEDYBEEF405WING` (release hex) | a CLI script over the MSP UART (`config-cli=`, or `config_cli` from the bench), saved; the ACC calibration for a plane; snapshot of sector 11 (in memory and `cfg-cache=`) | USART6 | SysTick VAL reads |

FDMs (`docs/fdm.md`): the RS7 bench runs the heli (`fdm: "heli"`), the trainer the plane (`fdm: "plane"`), both
through `hw/arm/fb_fdm.c.inc`; the aircraft file's `fdm` picks the type when `fdm=on`. The FDM's FRD body frame
reaches the IMU chip through the firmware's board frame (X forward, Y left, Z up) turned back by the
board's IMU alignment (`FbBoard.imu_align`: Nexus-XR CW180, F405 WING CW270 = `IMU_ICM42605_ALIGN`;
checked: 10° nose up reads `MSP_ATTITUDE` pitch −10.1, iNav's nose-up sign).

**speedybee-f405-wing** (`boards/speedybee-f405-wing/README.md`; evidence `evidence/f405/README.md`):
1 MiB flash with the F405 sector table (4 × 16 K, 64 K, 7 × 128 K; iNav's config = sector 11 at
0x080E0000, erased and programmed as `config_streamer_stm32f4.c` does), aliased at 0; SRAM 128 K,
CCM 64 K, BKPSRAM; system memory with the UID at 0x1FFF7A10 and the flash size at 0x1FFF7A22; 82
IRQs; bit-band aliases; RCC for the F4 tree (RDY follows ON, SWS follows SW): 168/42/84 MHz, APB1
timers 84, APB2 timers 168 MHz. USART1-3, UART4/5, USART6 with the F2/F4 register layout (QEMU's
`stm32f2xx_usart`). SPI1 (F4 variant: one RX frame, DFF 8/16) with the ICM-42688-P on CS PA4, polled
(no INT: iNav has no EXTI on this target; it runs its ICM42605-family driver). SPI2 with the MAX7456
on CS PB12 (`devices/include/fb/max7456.h`: iNav's OSD driver polls the chip until its clear bit
drops and would spin on an empty bus). SPI3 an empty bus (the SD card slot: `SD card: Startup
failed`). I2C1 is the STM32F4's v1 controller (`hw/arm/stm32_i2c_v1.c.inc`, host-tested under iNav's
polled driver by `tests/test_i2c_v1.c`) with the onboard SPL06-001 at 0x76 (`baro=`) and, wired, the
GPS module's QMC5883L at 0x0D (`mag=`): iNav's AUTO finds both (`BARO=SPL06, MAG=QMC5883`). A u-blox
GPS on a USART (`gps=uart3`, the target's GPS port) through an in-process wire at the receiver's baud,
BRR writes telling it the FC's baud; the FDM's NED position around `env` `home`. TIM2/3/4/8/12 with
per-timer clock and CCR: S1 (TIM4_CH2) drives the ESC as a pulse width (PWM, OneShot125 or Multishot by the width, no DMA), S1..S11
servos. A pad carries the pulse only when it is in the timer's alternate function (the pad table in
`stm32_soc.c.inc` has pin and AF: TIM2 AF1, TIM3/4 AF2, TIM8 AF3, TIM12 AF9), the channel output is
enabled (CCxE, CCxNE for S7's TIM8_CH2N) and, on TIM8, BDTR.MOE is set (see "Pad gating" below): iNav
without `feature PWM_OUTPUT_ENABLE` leaves the pads as GPIO outputs driven low
(`drivers/pwm_output.c:189-196`), and the bench then sends nothing, as the board. ADC1 with DMA2 stream 0 as iNav's `adc_stm32f4xx.c` programs it (the regular sequence, scan +
continuous, a circular buffer of 20 samples per channel): the battery (`battery=on`) on VBAT (PC0,
11:1) and CURRENT (PC1, 19.5 mV/A) — `Voltage: 16.79V (4S battery - OK)`; without it 0 V (`0S battery
- NOT PRESENT`). USB OTG FS is an unimplemented region (the VCP is not emulated; MSP/CLI go over
USART6). The control server polls `MSP2_INAV_STATUS` (MSP v2) for `fc.armed`, `mode`,
`arming_flags`, `cpu_pct` (RotorFlight's decoder stays for nexus-xr), `MSP_ALTITUDE` while iNav
reports the baro, `MSP_RAW_GPS` while it reports the GPS (`fc.gps`). iNav with the GPS and the compass
has 47 mode boxes and refuses `MSP_BOXNAMES` (the names do not fit its reply): the core then names
the boxes from `MSP_BOXIDS` and iNav's permanent-id table (`fb_msp_modes.c.inc`). Evidence:
`evidence/sensors/README.md`.

**The CLI configuration path.** Boards with `"config": "cli"` in board.json get no blob: the resolver
concatenates the board's `defaults.cli`, the aircraft's `firmware_cli`, the bench profiles and the
wired devices' lines into one script (`config_cli`, `devices/src/session.c`). On a boot with a blank
config sector the machine waits until the firmware answers MSP on USART6, enters the CLI, types the
script line by line with the prompt handshake (each line logged as `config: <line>`; a rejected line
is a `config: error` on the control socket and in the log, and nothing is saved), then `save`; the
firmware writes sector 11 and reboots. That sector is then kept in memory as the snapshot keyed by
firmware hash + script hash: an MCU reset or a power cycle boots from the flash as it is, and a
`firmware` load (which erases the whole flash) or a `reset` that erases the config area writes the
snapshot back instead of running the script again. The snapshot is also written to
`-M cfg-cache=<dir>` (default `<user cache dir>/flightbench/config` — `~/.cache` on Linux,
`%LOCALAPPDATA%` on Windows —, `off` = memory only; one file
per machine + key), so a new core restores it at its power-on instead of typing the script again.
With aircraft functions marked `auto_first_start` (`aircraft.json` `functions`; the trainer's
`calibrate_acc`) the flow has more steps before the snapshot: after the script's save (a reboot) the
core runs those functions one by one (the ACC calibration: the poses scenario on the FDM,
`MSP_ACC_CALIBRATION` at each of its six poses, `MSP_CALIBRATION_DATA` until iNav has that position),
then enters the CLI again and saves; the key then also covers the functions' names and scenarios
and the IMU's static error (`models/trainer/README.md`). Before the final `save`, in the same CLI
session, the core runs `diff all` and keeps the text with the snapshot (`<file>.diff.txt` next to the
`.bin` in `cfg-cache`): the `diff` message (`docs/protocol.md`), sent again with the snapshot when a
later start restores it.

**The bench state** (`-M bench-state=<file>`; `run.sh --session` passes
`<cfg-cache>/<bench file name>.state.bin`, `BENCH_STATE=` overrides; the evidence scripts pass none).
The firmware's config belongs to the bench: the flash controller reports every save the firmware
makes (the flash locked again after erasing/programming sector 11: CLI `save`, `MSP_EEPROM_WRITE`
from the configurator, the calibrations' own saves), and after each the sector goes to `<file>` and
its metadata to `<file>.json` minus `.bin` (firmware hash, machine, the script the sector has, the
automatic functions whose results it holds with the key of their inputs, the last `diff`, the time),
each written to a temporary file and renamed. At the next start the bench state is written back
instead of the snapshot (same machine + firmware hash, else ignored with a log line), then only the
script's added/changed lines against the metadata's script are typed (+ `save`), removed lines are
reported, not reverted, and only the automatic functions whose results are missing run (the
firmware's calibration flags, a function never run on the bench, changed inputs). The snapshot stays
the reproducible baseline (`reset all` restores it; `config reset` runs the first start again and
discards the bench state). Protocol: `docs/protocol.md` `event config`, `cli`, `config`. Evidence:
`evidence/bench-state/` (`scripts/bench_state_evidence.py`).

**The script's layers** (branch `config-layers`, 2026-09-30). Every line of a CLI board's script
carries its layer and source (`devices/src/session.c`: `board` base script, `aircraft`
`firmware_cli`, `profile`, `device` "device ublox on uart3", `bench` = the bench's own `bench_cli`
file after the device lines; `disabled_lines` skips lines by text or `layer:index`); the same
setting in two layers with different values is a conflict (reported; the later line wins, as the
firmware applies them), by the settings normaliser `devices/src/cli_norm.c` (Python reference
`scripts/cli_norm.py`, equal on every bench and on `tests/fixtures/cli_diff/`). `session-check`
reports `config_src:` / `config_disabled:` / `config_conflict:` lines and one `config_json:` line;
the machine keeps the provenance (`fb_session_config_line`) for the `config` events' `sent` and the
comparisons. Explicit operations on the control server: `config apply` (the bench assembled again
from its files, the lines the sector lacks — or all — typed, `diff all`, `save`), `config snapshot`
(the firmware's `diff all` written to the file the app names: the bench's lines), `config export`
(the assembly, the script, whether the firmware has it, the last diff: what a preset packs), `diff
against:"script"` (the script's settings against the firmware's `diff all`: changed / only in the
firmware / only in the script, with each script line's layer and source). Precedence: the bench
state is the running config; a snapshot named as `bench_cli` is the starting diff of a fresh first
start. Protocol: `docs/protocol.md` (session description, report, `config`, `diff`). Evidence:
`evidence/config-layers/` (`scripts/config_layers_evidence.py`).

**Modes, diff, functions** (`docs/protocol.md` `modes`, `diff`, `function`; `hw/arm/fb_msp_modes.c.inc`
decodes, host-tested by `tests/test_msp.c` on replies captured from both firmwares in
`tests/fixtures/`). After every boot, once the firmware answers, the control server reads
MSP_BOXNAMES / BOXIDS / RX_MAP / MODE_RANGES (+ _EXTRA on RotorFlight) between its periodic polls
(the 10 Hz status/attitude/rc cadence is kept) and sends `modes`; again on request and ~0.3 s after
the MSP host's MSP_SET_MODE_RANGE / MSP_EEPROM_WRITE. `state.fc.boxes` = the active boxes as
permanent ids. `diff` on request: CLI, `diff all`, `exit` (a reboot); the MSP host socket is not read
meanwhile. The environment functions (`calibrate_acc`, `calibrate_gyro`, `launch`) run on the
`function` command with `function` progress messages. A client that connects late gets the last
`config` event, `function` message, `modes` and `diff` marked `"replay":true`. Evidence:
`evidence/modes/` (`scripts/modes_evidence.py`).

## Machine options (`-M nexus-xr,<option>=<value>,...`)
`-M nexus-xr,help` lists them. "bench" = set by `session=` from the bench (unless given explicitly: an
explicit option wins).
| option | what | bench |
|---|---|---|
| `session=<bench.json>` | the machine resolves the bench (wiring, options, blob, manifest; `devices/src/session.c`; `docs/protocol.md` "Launching the core"); a rejected bench: `session: ` lines, exit 2 | — |
| `root=<dir>` | data root of the bench (`boards/`, `models/`); default `$FB_ROOT`, else the nearest ancestor of the bench's directory with `boards/<board>/board.json` | — |
| `session-check=on` | print the resolved bench (report format in `docs/protocol.md`) and exit 0 (2 when rejected); without `session=`: the manifest line | — |
| `fw=<elf\|hex\|bin>` | firmware loaded into flash by the machine (hot-reloadable) | `firmware` |
| `config-cli=<file>` | the CLI config script of a `cli` board (speedybee-f405-wing), typed into the CLI on a blank config sector | in memory: `config_cli` |
| `defaults=<file>` | custom-defaults blob placed at the image's area (0x08002808) | in memory from `board_config`, `profile`, `profile_text`, device `config` |
| `manifest=<file>` | the core manifest sent after `hello` | in memory |
| `ctl=<host>:<port>` | control server address | no (the launcher's `ports.control`) |
| `aircraft=<id>`, `aircraft-file=<file>` | aircraft name in `hello`; `aircraft.json` (transmission, ESC, FDM parameters, FDM inputs) | `aircraft` |
| `fdm=on\|off` | the aircraft's FDM drives the IMU (the file's `fdm`: heli, plane) | `aircraft` with `"fdm":"heli"` (nexus-xr) / `"plane"` (speedybee-f405-wing) |
| `env-scenario=<name>` | plane: the environment scenario from the start and after `reset aircraft` instead of the aircraft's `env_default` | no |
| `cfg-cache=<dir>\|off` | CLI boards: the config snapshots on disk | no |
| `bench-state=<file>` | CLI boards: the bench's config sector (+ `.json` metadata), written after every firmware save, restored at start, the script applied as a delta | no (the launcher's; `run.sh --session`: next to the cfg-cache) |
| `imu-accel-error=<mg>:<pct>` | a static accelerometer error (every axis): offset and sensitivity | wiring (`icm42688p` `accel_offset_mg`, `accel_scale_pct`) |
| `imu=on\|off`, `imu-noise=<gyro_dps>:<accel_g>` | ICM-42688-P on SPI1; motion-source noise | wiring (`icm42688p`, its noise props) |
| `crsf=uart3..6\|off` | CRSF receiver's USART | wiring (`crsf`) |
| `esc=on\|off`, `esc-timeout-ms=<ms>` | ESC on MOTOR 1; signal-loss timeout | wiring (`esc`, `signal_timeout_ms`) |
| `baro=on\|off`, `baro-env=<p0>:<h0>:<t0>`, `baro-props=noise_scale=<k>` | the onboard baro at 0x76: DPS310 on I2C3 (nexus-xr), SPL06-001 on I2C1 (speedybee-f405-wing); its atmosphere; the SPL06's props | wiring (`dps310`, `spl06`), `environment` |
| `mag=on\|off`, `mag-props=<prop>=<v>:...` | a QMC5883L on I2C1 at 0x0D (speedybee-f405-wing): `mount`, `declination_deg`, `inclination_deg`, `intensity_ut`, `noise_mg` | wiring (`qmc5883l`) |
| `gps=uart1..5\|off`, `gps-props=<prop>=<v>:...` | a u-blox GPS on a USART (speedybee-f405-wing): the `ublox` descriptor's props | wiring (`ublox`) |
| `battery=on\|off`, `battery-props=<prop>=<v>:...` | a LiPo on the VBAT/CURRENT pads (ADC1; speedybee-f405-wing): `cells`, `capacity_mah`, `start_soc`, `resistance_mohm`, `current_scale`, `fc_current_a` | wiring (`battery`) |
| `servos=<mask>`, `actuators=<a:b:c:d:e>`, `servo-rev=<mask>` | servos on SERVO 1..4; actuator names of servo1..4, motor1; reversed outputs | wiring (`servo`, `actuator`, `reversed`) |
| `osd=uart3..6\|off` (nexus-xr), `osd=uart1..5\|off` (speedybee-f405-wing) | MSP DisplayPort OSD device's USART | wiring (`msp_osd`) |
| `osd-log=<file>` | every byte the firmware sends to the OSD device, raw (flushed per DRAW; the replay tests' streams) | no |
| `osd-poll=on\|off` | the OSD device's MSP_FC_VARIANT request every 125 ms (default on for speedybee-f405-wing: iNav draws only while polled; off for nexus-xr) | no |
| `beeper=<pin>[:inv]` | beeper pin (`resource BEEPER`) | board config / profiles |
| `realtime=on\|off` | start throttled to the host clock | `time.realtime` |
| `idle-skip=on\|off`, `idle-skip-n=<n>`, `idle-skip-gran-us=<us>`, `idle-skip-mode=budget\|halt`, `idle-skip-src=cyccnt\|systick` | the idle skip ("external WFI"; needs `-icount`); its trigger: DWT CYCCNT reads (RotorFlight) or SysTick VAL reads (iNav), default per board | `time.idle_skip`, `idle_skip_n`, `idle_skip_gran_us` |
| `io-tb=on\|off` | under `-icount`, one TB per device-I/O instruction instead of a rewind at every execution (`patches/icount-io-tb.diff`); faster MMIO polling, **not guest-identical to off** (the idle skip decides differently, see "Idle skip"); default off. A speed experiment only: sessions, the evidence scripts' gates and the app leave it off (its flights are not comparable with the baselines) | no |
| `prof=on`, `prof-period-s=<s>` | host-time ledger | no |
| `imu-log=<file>`, `fdm-log=<file>`, `fdm-log-ms=<ms>` | IMU trace, FDM CSV log | no |
| `imu-samples`, `imu-odr`, `bench` (read-only) | counters for evidence (`qom-get /machine ...`) | — |

## Build and run
```bash
sudo apt install ninja-build libglib2.0-dev libpixman-1-dev libslirp-dev pkg-config flex bison python3-venv gdb-multiarch
core/qemu/build.sh <qemu-src-dir>        # clones QEMU v11.1.1 there, patches, builds <qemu-src-dir>/build/qemu-system-arm
# arm-none-eabi objcopy / nm on PATH (the firmware's own toolchain works)
core/qemu/run.sh path/to/rotorflight_STM32F7X2_OSD.elf &
python3 core/qemu/scripts/cli.py 31235 6  # CLI status over USART1
# or the whole bench from a session description (the machine resolves it: -M session=), then talk
# to the control server:
core/qemu/run.sh --session core/qemu/sessions/rs7.json &
# check a bench without starting anything (the app does this): report on stdout, exit 0 / 2
<qemu-src-dir>/build/qemu-system-arm -M nexus-xr,session=core/qemu/sessions/rs7.json,session-check=on -display none
python3 tools/core_client.py dump --seconds 1          # hello, signals, 1 s of state
python3 tools/core_client.py send '{"type":"cli","text":"status"}' --wait 5
```
Boot sequence under gdb: `GDB=1 BOARD_CONFIG= core/qemu/run.sh <elf> &` then
`gdb-multiarch -batch -x core/qemu/scripts/gdb-boot.gdb <elf>`.

## Firmware: what is needed for MSP on a UART
The stock defaults put MSP on USB VCP only, and USB OTG is not emulated. Two things are needed,
both without touching the firmware source:
1. **Build with `OPTIONS="USE_MSP_UART"`** (`make TARGET=STM32F7X2_OSD OPTIONS="USE_MSP_UART"`).
   `src/main/io/serial.c` then also puts MSP on USART1 in the defaults. The output name and obj dir
   do **not** change with OPTIONS, so build in a separate copy of the tree (we used
   `git archive HEAD` + the working-tree diff of the firmware checkout into a scratch directory).
2. **The board config as custom defaults.** The build's `.custom_defaults` area (0x08002808–0x08004000)
   is empty; the configurator inserts the unified config there when it flashes a real board. Without it
   USART1 has no pins and is never opened. `run.sh` builds `boards/nexus-xr/NEXUS_XR.config` with
   the `## Rotorflight Custom Defaults` header into a blob that the machine writes at 0x08002808
   (`defaults=`), together with the firmware (`fw=`).

The machine loads the firmware into its flash itself (ELF PT_LOAD segments / Intel HEX / raw binary;
parsers in `devices/src/fw_image.c` with `devices/tests/test_fw_image.c`; only regular files up to
2 MiB) and puts the custom-defaults blob where the image's own pointers say (`__custom_defaults_start/
end` at 0x08002800, validated; 0x08002808..0x08004000 only as a logged fallback)
instead of `-kernel` + `-device loader`: those are ROM blobs that QEMU re-applies at every reset, which
would undo a firmware replaced at runtime. With `fw=` a reset touches no flash, like the real chip.
(`-kernel` still works when `fw=` is not given.) On first boot the firmware finds no valid EEPROM,
applies the custom defaults, writes the config sector at 0x08004000 (sector erase + word programming via
the FLASH controller) and does `NVIC_SystemReset`; the written config survives resets.

## Machine model notes
- Memory: ITCM RAM 16K @0, flash 512K @0x08000000 (rom_device: reads direct, writes via the FLASH
  controller) aliased at 0x00200000, SRAM 256K @0x20000000, system memory 1M @0x1FF00000 with UID and
  flash size (512) filled in.
- Cortex-M7 without TrustZone: the reset vector table comes from `init-nsvtor` (0x08000000), not
  `init-svtor` — setting the latter left SP/PC at 0 and the core locked up.
- FLASH controller: KEYR unlock sequence, CR.LOCK, sector erase (SER/SNB/STRT, F722 sector layout),
  mass erase, programming only with CR.PG, NOR semantics (bits can only be cleared). SR never busy.
- DWT: CYCCNT = virtual time × 216 MHz, enabled by CTRL.CYCCNTENA; placed in the armv7m container
  above its RAZ/WI PPB default region. DEMCR is RAZ/WI in QEMU's NVIC (logged as bad offset 0xdfc, harmless).
- USART1..6, UART7/8: QEMU's `stm32l4x5-usart` (same register layout as F7: ISR 0x1C, RDR 0x24, TDR 0x28).
  `serial_hd(0)` is USART1.
- RCC: storage with reset values; `xxxRDY` follows `xxxON`, `CFGR.SWS` follows `SW`, CSR reset flags
  (PORRSTF at power-on, SFTRSTF after a system reset, RMVF clears). PWR CSR1 reports VOSRDY/ODRDY/ODSWRDY.
- ADC: SR always 0x1E, JDRx 1489 (Vrefint at 3.3 V), DR 2048 — same as the Renode stub.
  Regular conversions are DMA-driven on this firmware, so `status` shows `Vref=0.0V`.
- SPI2..5: "empty bus" behaviour only — TXE always set, a DR write sets RXNE, a DR read returns 0xFF
  and clears it. Without it the gyro probe spins in `spiSequenceStart`. No device models, no DMA.
- Everything else in 0x40000000–0x4007FFFF, 0x50000000–0x5006FFFF, 0xA0000000–0xA0001FFF is a storage
  stub: reads return the last written value (0 after reset); the first access to each 1K block is logged
  with its peripheral name under `-d unimp`. RTC/backup registers and BKPSRAM survive system reset.


### Gyro: GPIO, EXTI, SPI1, DMA2, IMU (milestone 2 step A)
- **GPIO A..D** (`0x40020000`): MODER/OTYPER/OSPEEDR/PUPDR/ODR/LCKR/AFR as storage, BSRR sets/resets ODR,
  IDR = ODR for output pins and the externally driven level otherwise. Pin level changes drive the IMU
  chip select (PA4, active low, idle high when not an output) and the EXTI edge detectors. MODER/AFR
  also decide whether a timer output reaches its pad (Outputs, "Pad gating"); no other AF routing.
- **SYSCFG/EXTI**: EXTICR stays plain storage in the stub window and selects the port per line; EXTI
  IMR/EMR/RTSR/FTSR/SWIER/PR (write 1 to clear); `PR & IMR` drives IRQs 6..10, 23 (lines 5..9), 40 (10..15).
- **SPI1** (`0x40013000`): CR1/CR2/SR/DR/CRCPR. A DR write while SPE=1 clocks each byte through the
  selected device (the IMU when PA4 is low, `0xFF` otherwise) at once; MISO goes to a 4-byte RX FIFO.
  8-bit frames with data packing (a 16-bit DR access is two bytes, low first — the driver's polled path
  uses it), `SR.RXNE` honours `CR2.FRXTH`, FRLVL reported, TXE always set, never BSY.
- **DMA2** (`0x40026400`): RotorFlight always gets SPI DMA when a stream is free — `dmaopt = -1` means
  "try every option", the CLI only accepts valid options, there is no switch to turn it off — and after
  100 gyro interrupts (`GYRO_EXTI_DETECT_THRESHOLD`) it reads the IMU by EXTI → DMA (13 bytes
  ACCEL_DATA_X1..GYRO_DATA_Z0, `status` says `locked dma`). Modelled: stream registers, LISR/HISR and the
  clear registers, IRQs 56..60/68..70. When SPI1's CR2 has TXDMAEN (+RXDMAEN) and an enabled stream pair
  has PAR = SPI1_DR, the bytes move at once; completion (EN cleared, NDTR 0, HTIF|TCIF, IRQ if enabled)
  comes after the SPI bit time (`PCLK2 / 2^(BR+1)`, 13.5 MHz for this driver → ~7.7 µs). Other streams
  (ADC) are plain registers and never transfer, as before.
- **IMU**: `fb_icm42688p_*` from `devices/src/icm42688p.c`, only through its public API. The machine does
  CS edges, byte exchange, a virtual-clock sample timer at `fb_icm42688p_odr_hz()` (re-evaluated at every CS
  release, stopped at 0; 4 kHz on this config), the INT1 pulse on PB8 (`_int1_pulse_ns()`, polarity from
  `_int1_active_high()`; 8 µs active-high as this driver sets it). The chip keeps its state across an MCU
  reset. Counters: `qom-get /machine imu-samples`, `qom-get /machine imu-odr`. `-M imu-log=<file>`: one
  line per event the firmware can see, virtual ns — `S <t>` INT1 rises (a sample), `F <t>` INT1 falls,
  `D <t> <stream>` an SPI1 DMA stream completes (`scripts/speed_evidence.sh` stage I compares builds).
  Each sample costs three deadlines (sample, pulse end, DMA completion); removing either of the last two
  was tried and made the core slower (`evidence/speed/README.md`, fix 3): they also stop the idle skip
  in front of the scheduler's gyro target.
- **Motion source**: chip-frame rate/specific force/temperature plus per-sample Gaussian noise and a slow
  sinusoidal temperature drift. Default = hover: 0 deg/s ± 0.05 deg/s RMS, (0, 0, +1 g) ± 2 mg RMS,
  30 °C ± 1 °C over 600 s; xorshift PRNG with a fixed seed, so icount runs are reproducible. Noise is not
  decoration: noiseless data would not exercise the firmware's calibration the way a real sensor does.
  Runtime control: one JSON object per line on the `fbmotion` chardev (`run.sh`: TCP 127.0.0.1:31237),
  any subset of `gyro_dps[3]`, `accel_g[3]`, `temp_c`, `gyro_noise_dps`, `accel_noise_g`, `temp_swing_c`;
  reply `ok` or `error: ...`:
  ```bash
  printf '{"gyro_dps":[10,-20,30]}\n' | nc -q1 127.0.0.1 31237
  ```
- **Axes**: `gyro_1_sensor_align = CW180` (with `gyro_1_align_yaw = 1800`) is a 180° turn about Z, so
  board = (−x, −y, +z) of the chip. Chip +Z is board +Z (up), hence the hover default puts +1 g on chip Z.
  Checked: chip (10, −20, 30) deg/s reads back as board (−10, +20, +30) deg/s in `MSP_RAW_IMU`.

### Outputs, receiver, ESC and FDM (milestone 2 step B)
- **Timers TIM1/TIM2/TIM3** (the RS7's outputs): all registers as storage, CNT counting in virtual time at
  216 MHz / (PSC+1) modulo ARR+1 (RotorFlight's `timerClock()` is `SystemCoreClock` for every F7 timer),
  SR write-0-to-clear, EGR.UG restarts the counter. No interrupts, no pin driving, no ARR/PSC preload.
  Servo pulse width = CCRx · (PSC+1) / 216 MHz, read by the physics tick (SERVO 1..4 = B04 TIM3_CH1,
  B05 TIM3_CH2, B00 TIM3_CH3, A15 TIM2_CH1). With the RS7 profile the servo timers run at 8 MHz
  (475 Hz update rate; 560 Hz is capped by the firmware for 2000 µs pulses), the tail at 536 Hz.
- **DShot600 on TIM1_CH2 (A09)**: a DMA2 stream memory→TIMx_CCRy runs when the timer's CCyDE is set.
  The buffer (16 bit slots + 2 reset slots, 32-bit words) is taken at once, decoded with
  `fb_dshot_decode_ccr()` against ARR+1 = 20 ticks (TIM1 at 12 MHz), both checksum polarities tried, and
  the stream completes after 18 bit periods (TCIF, IRQ 69). DShot value, frame and bad-frame counters
  are in the `bench` property.
- **Bidirectional DShot reply**: the firmware's TC handler turns the pin into an input capture with a
  peripheral→memory stream on the same CCR (22 slots). 30 µs later the machine writes the ESC's eRPM as
  capture timestamps: `fb_esc_erpm()` → `fb_dshot_erpm_to_value16()` → `_value16_to_stream21()` →
  `_stream21_to_edges()` with 4/5 of the output bit period (16 ticks per GCR bit, the firmware decodes
  16 ticks per bit), 16-bit wrap like TIM1, NDTR counting down per edge. The firmware's own decoder
  reports 0 % invalid packets (`dshot_telemetry_info`).
- **Pad gating** (both boards, `hw/arm/fb_outputs.c.inc`, host-tested by `tests/test_outputs.c`): an
  output (servo or ESC pad) carries the timer's waveform only when its GPIO is in alternate-function
  mode (MODER 10) with the channel's AF number (the board's pad table: pin, timer, channel, AF), the
  counter runs (CR1.CEN), the channel output is enabled (CCER CCxE, CCxNE for a CHxN output) and, on
  TIM1/TIM8, BDTR.MOE is set. Otherwise the pad has no pulse: a servo gets no pulse and holds its last
  position (the FDM keeps the last deflection), a pulse ESC (the F405's S1) gets nothing and a DShot
  frame (the Nexus-XR's TIM1_CH2) is clocked out by the DMA but never reaches the ESC, whose signal-loss
  timeout then stops the motor. The state reports such a wired output as `null` in `pwm_us` and by name
  in `no_signal` (`docs/protocol.md`). Checked with iNav's `feature -PWM_OUTPUT_ENABLE`:
  `evidence/pin-mode/`.
- **ESC signal loss**: no DShot frame for `esc-timeout-ms` (default 100; session wiring
  `esc.signal_timeout_ms`, 0 = never) → throttle 0, the motor spins down (FC hung, rebooting, off).
- **ESC + motor**: `fb_esc` (parameters from the aircraft file `esc`: RS7 kv 520, 10 poles, τ 0.4 s,
  200 A, 50 V) gets every decoded DShot value and steps at 1 kHz. It is the **one source of head speed**:
  the FDM's rotor turns at motor rpm / gear (aircraft file `transmission.main_rotor_gear_ratio`, RS7
  `[13, 116]` as in the profile), the model's stand-in motor is bypassed
  (`fb_heli_set_rotor_rpm()`), and the rotor's drag goes back to the ESC as load
  (`fb_heli_rotor_load()`), so the firmware's governor regulates a motor that droops under collective.
- **CRSF receiver on UART5** (`serial 4 64`, C12/D02; the Nexus-XR's built-in ELRS; `-M crsf=uartN|off`
  puts it on another USART or removes it): `fb_crsf` behind an in-process chardev
  (`chardev-fbwire`): one virtual-time deadline per frame (`crsf.frame`, on the model's packet-rate grid,
  `fb_crsf_next_due_ns()`), not one per 420 kbaud byte: the frame's first byte enters RDR at the frame
  time, each next one right after the firmware's RX interrupt consumed the previous one (its handler reads
  RDR, then writes `RQR.RXFRQ`; that write releases the next byte, so the flush never eats it). The
  RXFRQ write is only an early trigger: a driver that never writes it (polling, another HAL) gets each
  byte at its 420 kbaud arrival time (the timer is armed at the next byte's arrival while bytes are
  queued and moved to the next frame once the RXFRQ path drained them, so RotorFlight still costs one
  deadline per frame). The firmware sees the frame's bytes a few µs apart instead of 23.8 µs; bytes never
  read by the next frame time are dropped (`crsf_dropped` in `bench`), as overrun would, and with the
  USART's receiver off (CR1 UE/RE) at once. FC→RX bytes go to the model's telemetry
  parser. 250 Hz RC frames, link statistics every 200 ms.
  Default sticks: all 1500 µs, channel 5 (ARM) and 6 (throttle) at 1000.
- **Pilot socket** (`fbpilot` chardev, `run.sh`: TCP :31238), one JSON object per line:
  `{"ch":[1500,...]}` (first N channels), `{"set":{"6":2000}}` (1-based), `{"link":false}`, any of them
  with `"at": <virtual seconds>` to queue it (applied by the receiver model at that virtual time — a
  whole flight plan sent up front is deterministic under icount). Reply `ok t=<virtual s>` or `error: ...`.
- **FDM** (`-M nexus-xr,fdm=on`): every 1 ms of virtual time `fb_fdm_step()` with the servo pulses by
  actuator name (a channel without pulses holds its last value) and the head speed from the ESC model; its
  body rates and specific force become the IMU motion source (noise and ODR sampling as before).
  Axes: FDM body FRD → board frame (x, −y, −z) (+Z up, right-handed like the chip) → chip through
  CW180 (−bx, −by, bz). Checked in flight: the firmware's heading follows the FDM's, roll/pitch
  doublets damp out. `fdm-log=<file>` writes a CSV every `fdm-log-ms` (default 10):
  `t,alt,vz,head_rpm,motor_rpm,throttle,dshot,roll,pitch,yaw,servo1..4,coll,on_ground,in1..in4`
  (`servoN` = the firmware's output on SERVO N, `inK` = what the FDM got as its input K after the wiring
  map below).
- **`bench` property** (`qom-get /machine bench`): one line with virtual time, servo µs, DShot value,
  frame/reply counters, ESC throttle/rpm/eRPM, head rpm, receiver telemetry frames, altitude.

### Baro: I2C3 + DPS310/SPL06 (branch `baro`, 2026-09-26)
The Nexus-XR's SPL06-001 is register-compatible with the DPS310 and RotorFlight detects it with
`barometer_dps310.c` (`baro_hardware = AUTO`, `baro_bustype = I2C`, `baro_i2c_device = 3`,
`baro_i2c_address` 0 → the driver's 0x76; I2C3 SCL A08, SDA C09).
- **What the firmware does on F7** (`drivers/bus_i2c_hal.c` → ST HAL `stm32f7xx_hal_i2c.c`): init
  writes TIMINGR (from PCLK1 54 MHz and the bus clock), OAR1/OAR2, CR2 AUTOEND|NACK, enables PE and
  NVIC EV/ER (IRQs 72/73). Register writes and reads at detect/configure are **blocking**
  `HAL_I2C_Mem_Write`/`HAL_I2C_Mem_Read`: START + address with the register byte (Mem_Write: RELOAD,
  wait TCR, then NBYTES = 1 AUTOEND; Mem_Read: SOFTEND, wait TC, then a repeated START with RD_WRN
  and AUTOEND), polling ISR TXIS/RXNE/TC/TCR/STOPF/NACKF against `HAL_GetTick()` timeouts, STOPF
  cleared through ICR. The cyclic read (`dps310ReadUP` → `i2cReadBuffer`) is
  **`HAL_I2C_Mem_Read_IT`**: the register address polled as above, then the read runs in the EV
  interrupt (RXIE/STOPIE/NACKIE/TCIE/ERRIE: RXDR per byte, completion on STOPF). No DMA (TXDMAEN/
  RXDMAEN never set), 7-bit addresses, master only. `baroUpdate` is a state machine in the BARO task
  (pressure start → 45 ms → read → sample → temperature steps), i.e. one 6-byte PSR/TMP read per
  ~47 ms; the first 100 pressures after boot are averaged into the ground pressure.
  `status` counts every HAL error in `I2C Errors` — including the NACKs of the auto-detect probes
  for chips that are not there (BMP085 and MS5611 at 0x77 before the DPS310), as on real hardware.
- **I2C3 model** (`0x40005C00`; `hw/arm/stm32_i2c.c.inc`, the machine only adds MMIO, the
  virtual-clock timer and the NVIC lines; host tests `tests/test_i2c.c`: NBYTES = 0 probes,
  data-byte NACK, RX RELOAD chains of 300/600 bytes, STOP mid-transfer, START while BUSY / during
  a STOP, PE = 0 mid-transfer, Mem_Write_IT / Mem_Read_IT interrupt sequences with the DPS310):
  CR1/CR2/OAR1/OAR2/TIMINGR/TIMEOUTR storage, ISR/ICR, RXDR/TXDR.
  START → address phase (10 bit times) → ACK: TXIS (write) or the first received byte (read); NACK
  from an absent address: NACKF, then the automatic STOP → STOPF, BUSY released. NBYTES counted per
  byte (9 bit times): at the end RELOAD → TCR (a CR2 write with NBYTES continues), AUTOEND → STOP →
  STOPF, else TC (the master writes START again or STOP). NACK on a data byte: NACKF, automatic
  STOP. Clock stretching: the next byte is received only after RXDR is read, also across a RELOAD
  (NBYTES reprogrammed before the chunk's last RXDR read: the bus waits, RXDR is never overwritten).
  START/STOP are set by software and cleared by hardware, ignored while PE = 0: START on an idle bus
  or after TC/TCR starts at once, during a STOP it follows that STOP, during a transfer it becomes
  the repeated START at the end of NBYTES (instead of TC); STOP while the software is due (TC/TCR,
  TXIS, RXDR full) is sent at once, while a byte is on the wire after that byte (the transfer ends
  there), on an idle bus it is ignored; neither bit stays set. CR1.PE = 0 resets the state machine
  and flags; ISR.TXE written 1
  flushes TXDR. EV IRQ = TXIE·TXIS | RXIE·RXNE | NACKIE·NACKF | STOPIE·STOPF | TCIE·(TC|TCR), ER IRQ
  = ERRIE·errors. Bit time from TIMINGR: (PRESC+1)·(SCLL+1+SCLH+1)/PCLK1 (400 kHz if 0). Not modelled
  (first use logged under `-d unimp`): DMA requests, 10-bit addressing, slave mode, SMBus/PEC,
  timeouts; the first NACK per address is logged too. I2C1/2/4 stay storage stubs.
- **Bus**: a slave table (7-bit address, byte-level start/write/read/stop callbacks); the DPS310 at
  0x76 through `fb_dps310_i2c_start/_write_byte/_read_byte` (pointer byte, auto-increment). A
  compass (or anything else) on the bus is not modelled: the session check reports it and leaves it
  out, its address NACKs.
- **Baro**: `fb_dps310` from `devices/`, sampled by a virtual-clock timer at `fb_dps310_rate_hz()`
  (re-read at every STOP; 32 Hz as the driver programs it); a one-shot measurement (MEAS_CTRL 1/2,
  not used by RotorFlight) is sampled once after its datasheet conversion time
  (`fb_dps310_oneshot_us()`, 3.6..206.8 ms by oversampling), sets its RDY, the chip returns to idle.
  What it measures: every physics tick the machine computes the ICAO standard atmosphere at
  h = field elevation + FDM altitude, p = p0·(1 − 2.25577e-5·h)^5.25588, t = t0 − 6.5 K/km·h
  (`-M baro-env=p0:h0:t0`, session `environment`, default 101325 Pa, 0 m, 22 °C; p0 and t0 are
  **sea-level** values, QNH-style: t0 = 22 °C at a 1000 m field gives 15.5 °C there); each chip sample latches it plus the datasheet's
  pressure noise for the programmed oversampling (16x: 0.35 Pa RMS, fixed seed). The noise matters:
  with an exact 101325 Pa on the pad RotorFlight's ground offset is exactly 0.0f and `position.c`
  (`if (alt.haveBaroAlt && alt.baroAltOffset)`) then reports altitude 0 forever. The chip keeps its
  state across an MCU reset (power-on resets it). Fault `dps310.freeze`: the result registers stop
  updating, and keep the frozen bytes through the driver's SOFT_RST and a power cycle (sampling goes
  on, nothing is latched; a one-shot never completes). `-M baro=off` (session: `{"device":"dps310","detached":true}`) = 0x76 NACKs.
- **Signals**: `baro.pressure_pa`, `baro.temp_c` (what the chip is given, before its noise),
  `fc.baro_alt_m` and `state.fc.baro_alt_m` = MSP_ALTITUDE (the firmware's estimate: PT2-filtered
  baro altitude minus the ground offset it tracks while disarmed). The core polls MSP_ALTITUDE only
  while the firmware reports the baro in MSP_STATUS (sensors bit 1, `sensors(SENSOR_BARO) << 1`):
  wired but not detected (or detached) → no poll, `fc.baro_alt_m` absent; the poll cycle is then 4
  commands at 25 ms (~10 Hz each, as before). `bench`: `baro`, `baro_pa`, `baro_rate`,
  `baro_samples`, `i2c3_xfers`, `i2c3_nacks`, `i2c3_bytes`, `baro_regs` (the chip's PSR/TMP result
  bytes, hex), `baro_reg_pa` (the pressure the driver computes from them). The FDM log
  (`-M fdm-log=`) ends with `baro_reg_pa,fc_baro_alt` (the chip's pressure incl. its noise, the
  firmware's MSP_ALTITUDE; `nan` without a baro / before the first answer): the deterministic
  baro trace of a run.

Evidence `evidence/baro-2026-09-26.log` (`scripts/baro_evidence.sh`; own core on 31280..31284,
`qemu-baro` build; base = `main` deea543 built in the same tree, I2C3 a storage stub there):

| # | check | result |
|---|---|---|
| A unit tests, manifest | `make -C devices test` (incl. `test_dps310`: byte-level transactions, noise 0.35 Pa RMS, freeze), `make -C fdm test`, `test_wiring.py` 36 OK; manifest `dps310` `implemented:true`, faults `[freeze]`, signals `baro.pressure_pa`/`baro.temp_c`; pad `i2c3` onboard dps310 detachable, `devices:["dps310"]` | pass |
| B boot, pad | `status`: `Devices detected: SPI:1, I2C:1`, `GYRO=ICM42688P, ACC=ICM42688P, BARO=DPS310`, `I2C Errors: 2` (the auto-detect probes at 0x77 for BMP085/MS5611 that are not there; the same after a 62 s flight), arming flags only `CLI`; `tasks`: BARO 43 Hz (the task counts its state-machine steps; one pressure read per ~47 ms), max 22 µs; chip at 32 Hz (602 samples in 17 s), 747 I2C transactions; `baro.pressure_pa` 101325.0, `baro.temp_c` 22.00 at FDM altitude 0; firmware altitude on the pad −0.05..+0.05 m after calibration | pass |
| C flight | base build (no baro): lift-off 33.82 s, governor 1793–1802, FDM \|roll\| ≤ 8.37°, \|pitch\| ≤ 9.17°; this build with the baro: lift-off 33.82 s, governor 1793–1802, 8.38°/9.17°, hands-off MSP ≤ 1.0/0.4° (the same). Climb 0 → 3.46 m (t 34..61): \|fc.baro_alt − fdm.alt\| max 0.15 m, rms 0.04 m; against the FDM 0.15 s earlier (the firmware's filter + ~10 Hz poll) max 0.08 m, rms 0.02 m; table every 1 s in the log | pass |
| D detached | `baro=off`: `status` without BARO, `I2C:0`, `I2C Errors: 11` at boot (every driver's probe incl. the DPS310's 5 retries) and 11 after the flight (no flood: the BARO task is not scheduled), no BARO in `tasks`; lift-off 33.82 s, governor 1793–1802; `fault dps310.freeze` → `device dps310 is not wired in this session`; `fc.baro_alt_m` absent | pass |
| E freeze | liftoff plan + collective 1660 at 48..56 s (climb ~2.4 m/s, firmware 0.3 m behind the FDM); freeze 50..54 s: firmware altitude 6.21 m constant from 50.5 s while the FDM climbs 7.41 → 15.58 m; off: within 0.5 m of the FDM 0.44 s later, 57..62 s max 0.08 m | pass |
| F speed | host s per 10 virtual s in flight (40..62 s, no polling, idle skip on): baro on 15.9, 15.6; detached 15.6, 15.5 → ~+1 % (~0.2 s). FDM log to 62 s identical between the two runs of each (0f48c525426c on, 8d1253d713a8 detached; they differ from each other: the firmware runs another task) | pass |
| G ctl/fault | `ctl_evidence.py` all stages as before (realtime 0.250×/0.100×, reset/power/firmware/watch, CLI `get name = BENCH`); `fault_evidence.py` SUMMARY 1a..3 all PASS (1c now with `dps310.freeze`, `implemented`; i2c3 slot lists `dps310`) | pass |

Review fixes (branch `baro-2`, `qemu-baro2` build; the whole run again under `## review fixes` in the
log): A adds `test_i2c` (68 checks) and the new `test_dps310` checks; B pad −0.04..+0.05 m; C climb
\|fc − fdm\| max 0.17 m, rms 0.04 m (0.15 s lag: max 0.08, rms 0.02); D as before; E freeze in the
climb as before, and across an MCU reset and a power cycle the chip's PSR/TMP bytes stay the frozen
ones (`bench baro_regs`), live again after off; F 15.4–15.6 s per 10 virtual s, baro trace md5
7f1b24c46d4b in both runs (identical; FDM log cacbf93ddc53 both runs — it changed from 0f48c525426c
because MSP_ALTITUDE now waits for MSP_STATUS to report the baro), detached 8d1253d713a8 as before;
G all PASS.

Not modelled: SPI2..5 devices, DMA1, DMA for anything but SPI1 and timer CCRs, timers other than
TIM1/2/3 (GYRO_CLK on TIM11 — the driver enables CLKIN on the chip, the model ignores it), timer
interrupts, I2C1/2/4 (storage stubs; I2C3 with the baro: above), USB OTG, mag.

### MSP DisplayPort OSD (branch `osd`, 2026-09-26)
The firmware's own OSD in the app: device `msp_osd` (`devices/src/msp_osd.c`, category `osd`, one
`uart` port both ways) = the OSD side of an HD VTX (HDZero / DJI / Walksnail class), wired to a free
UART (`{"device":"msp_osd","port":"uart","pad":"PORT-A"}`; uart3..uart6, `-M osd=uartN`, never the
receiver's USART). The protocol side (`osd` message, attr bits, canvas) is in `docs/protocol.md`.
- **Firmware side** (RotorFlight 4.6, `io/displayport_msp.c`, `msp/msp_serial.c`): the MSP
  DisplayPort pushes MSP v1 `$M>` frames of MSP_DISPLAYPORT (182) with `mspSerialPush()` to the MSP
  port whose identifier is `displayport_msp_serial`; that port needs `FUNCTION_MSP` (bit 0, value 1)
  — RotorFlight 4.6 has no `FUNCTION_VTX_MSP`, and `config.c` resets `displayport_msp_serial` to
  -1 if the port has no MSP. Each OSD frame (`osd_framerate_hz` 12) is HEARTBEAT, CLEAR_SCREEN,
  WRITE_STRINGs, DRAW_SCREEN. This build (STM32F7X2_OSD: `USE_OSD`, `USE_MSP_DISPLAYPORT`,
  `USE_OSD_HD`, no MAX7456) already has `FEATURE_OSD` on and `osd_displayport_device = AUTO`, so
  without the device the OSD task runs and pushes to port -1 (nowhere): `status` says `OSD: MSP`.
- **CLI lines** (descriptor `config`, appended to the custom-defaults blob by the bench resolver only when
  the device is wired; `{serial}` = the pad's serial index, PORT-A = 3): `serial 3 1 115200 57600 0
  115200`, `set displayport_msp_serial = 3`, and a layout for the 50 × 18 canvas (x has 5 bits:
  columns 0..31; value = x | y << 5 | 2048 for OSD profile 1): flight mode (1,1), timer 1 = on time
  (24,1), timer 2 = armed time (24,2), DISARMED (21,4), crosshair (24,8), warnings (19,12), ESC rpm
  (1,15), battery voltage (1,16), debug (1,17: `debug_mode = GOVERNOR` of the RS7 profile → head
  speed target/current), and `set osd_displayport_device = AUTO` (below).
- **Canvas**: on each contact the device sends `$M<` MSP_SET_OSD_CANVAS (188) `[50, 18]`. RotorFlight
  stores it in `osdConfig.canvas_cols/rows` (no CLI setting; default 53 × 20) and, because the OSD
  was not yet HD + MSP, sets `vcd_video_system = HD`, `osd_displayport_device = MSP`, saves and
  reboots: one extra `reboot {cause:reset}` at 1.56 s on a fresh flash; from the next boot on the
  display is initialised at 50 × 18 (MSP_OSD_CANVAS 189 → 50, 18). With MSP + HD already set that
  save is skipped (`msp.c`: it saves only when one of the two changes), the size stays in RAM only
  and reaches the display only if it arrives before the OSD task's first redraw (a race). So the
  device's lines set `osd_displayport_device = AUTO` explicitly; they come after the session
  profiles in the custom defaults, so a profile (a pasted `diff all` from a real HD quad) that set
  MSP cannot skip the save + reboot on a fresh flash. `session.py --check` prints the line.
- **Machine** (`nexus_xr.c`): the device sits behind an in-process `chardev-fbwire` like the CRSF
  receiver; FC bytes go to `fb_msp_osd_rx_byte()`, each DRAW is handed to the control server
  (`ctl_osd_frame`), the device's own bytes enter the USART one per 115200 byte time. The VTX is
  reset with the board on a power cycle; every MCU reset is reported to it
  (`fb_msp_osd_contact_lost()` from `nexus_xr_reset`) → the next frame is a new contact, canvas
  announced again, next frame full. Silence also ends the contact, adaptively: after max(250 ms,
  3 × the frame interval) (the longest of the last 8 DRAW gaps ≥ 16 ms; RotorFlight's 3 DRAWs
  within 2 ms at boot are not frames; 1 s until measured), so `osd_framerate_hz = 2` (500 ms
  apart) does not flap; a timer (`osd_watch`) notices the silence without waiting for the next
  byte. The parser reads the MSP v1 jumbo form (size 255 + 16-bit size; > 256 bytes read and
  dropped) and rescans after a bad checksum from the next `$`, so a frame cut by a reset does not
  swallow the next one. `bench`: `osd`, `osd_frames`,
  `osd_writes`, `osd_clipped`, `osd_contacts`, `osd_canvas_sent`, `osd_canvas_acks`, `osd_bad`.
- **Control server**: one `osd` message per DRAW that changed a cell (since the iNav work: a DRAW
  that changes nothing is not sent to a client in sync), runs of the changed cells or the whole canvas
  (`clear:true`: new contact, a client that connects or turns `osd` on); per client `osd` defaults
  to on and is never dropped. The cells are encoded by the device library
  (`fb_msp_osd_cells_json`, unit-tested). `link:false` (clear, no cells) once when the picture is
  gone: power off, MCU reset, silence past the threshold, `msp_osd.link_lost` on, the firmware in
  CLI mode (its DisplayPort sends nothing while `cliMode`; a DRAW still in the FIFO is dropped);
  the next real frame is full with `link:true`. Signal `osd.frames`, fault `msp_osd.link_lost` (the
  device ignores the FC and sends nothing; off → full frame). One `msp_osd` per session (a second
  entry is a `WiringError`).
- Not rendered: MSP_DP_SYS elements (goggle/VTX voltage, bitrate, ...: counted only); MSP v2
  DisplayPort frames (RotorFlight pushes v1). RotorFlight 4.6 draws the boot logo (3,1) and `ARMED`
  (12,7) at fixed SD positions, left of centre on the HD canvas. The battery voltage is 0.00 V (the
  ADC is not emulated), so the firmware blinks it (attr 0x40, shown every other 250 ms).

Evidence `evidence/osd-2026-09-26.log` (`scripts/osd_evidence.sh`, own core on 31280..31284,
`qemu-osd` build; base = `main` 075239c built in the same tree):

| # | check | result |
|---|---|---|
| A tests, manifest | `make -C devices test` incl. `test_msp_osd` (the firmware's byte stream: canvas request bytes, full/delta, removed element = blank cells, attr/blink kept, clipping, coalesced DRAWs, bad checksum, silence → new contact, RELEASE, link_lost, reset), `fdm`, `test_wiring.py` 43 OK (6 new), `test_i2c`; manifest `msp_osd` category `osd`, port `uart` both, `implemented:true`, fault `link_lost`, signal `osd.frames`, `config`; uart3/4/5/6 list `msp_osd` | pass |
| B boot with the device | CLI `serial 3 1 115200 57600 0 115200`, `displayport_msp_serial = 3`, `osd_displayport_device = MSP` and `vcd_video_system = HD` (set by the firmware), `osd_framerate_hz = 12`, OSD task 12 Hz max 136 µs; MSP_OSD_CANVAS 50, 18; first message t = 1.39 `clear:true`, reboot 1.56, full again at 2.92, then deltas (0..156 cells, mean 4.8; 89 of 113 without a change on the pad); **12.00 Hz**, 83.2..83.5 ms apart; one full frame (sent on `subscribe osd:true`) as text art: ACRO (1,1), timers `\x9b00:10` / `\x9c00:00` (24,1/2), DISARMED, crosshair `rst` = 0x72..0x74 (24,8), rpm `0` (1,15), `\x960.00\x06` with attr 0x40 (1,16), `DBG 1800 0 0 0` (1,17); 0 clipped, 0 bad checksums | pass |
| C lift-off with the device | lift-off 33.82 s, head speed 1793..1802, governor 1793..1802, FDM 8.36°/9.16° (the same as without); DISARMED gone at 14.01 (armed 14.02), `ARMED` splash 14.09..14.59, then ACRO again; timer 2 counts from arming (00:47 at 62 s), timer 1 01:00; rpm 180 at 15.09 → 16080 in the hover (324 distinct values), DBG current head speed 1798..1801 in the hover canvases; 12.00 Hz armed and airborne | pass |
| D reset / fault / subscribe | `reset firmware` at 8.03: reboot 8.03, silence 1.36 s, first frame after it `clear:true` (empty), then the boot logo, `4.6.0` and `CMS: THR MID + YAW LEFT + PITCH FWD`; `msp_osd.link_lost` 1 s: 0 messages, off → next message `clear:true` (14 runs); `osd:false` 1 s: 0 messages, `osd:true` → full frame at once, then deltas; `osd:"yes"` → error | pass |
| E without the device | no `osd` message, `osd.frames` null, `fault msp_osd.link_lost` → `not wired`, bench osd 0; CLI `serial 3 0 …`, `displayport_msp_serial = -1`, `osd_displayport_device = AUTO`, arming flags only CLI; the session's -M string has no `osd`; lift-off 33.82 s, head speed 1793..1802, FDM 8.38°/9.17° (base build: 33.82 s, 1793..1802, 8.38°/9.17°); quiet runs: FDM log md5 cacbf93ddc53 = the baro-2 reference (bit-identical to main) | pass |
| F speed in flight | host s per 10 virtual s, 40..62 s, no polling: with the device 15.7, 15.8; without 15.9, 15.9 (no measurable cost; the firmware pushes to port -1 anyway). Boot + flight to 62 s: 116–117 vs 107–108 host s (the extra save + reboot). FDM log with the device 5a6812786d67 in both runs (deterministic; differs from without: an extra reboot and the UART traffic) | pass |
| G ctl/fault | `ctl_evidence.py` all stages as before (realtime 0.250×/0.100×, reset/power/firmware/watch, CLI); `fault_evidence.py` SUMMARY 1a..3 all PASS (1c with `msp_osd.link_lost`) | pass |

### MSP DisplayPort OSD on iNav (branch `inav-osd`, 2026-09-30)
The same `msp_osd` device on the speedybee-f405-wing (uart1..uart5; the app's
`inav-trainer-fpv-osd` preset and `sessions/trainer-osd.json`: uart4 = iNav serial 3). What iNav 9.1.0
does differently from RotorFlight, and what changed for it (details: `docs/protocol.md` `osd`,
evidence: `evidence/inav-osd/README.md`):
- **CLI lines per firmware**: the descriptor's lines moved to `config_firmware` (`rotorflight`: as
  before; `inav`: `feature OSD`, `serial <n> 33554432 115200 115200 0 115200` = FUNCTION_MSP_OSD alone
  on the port at the peripheral baud, `set osd_video_system = HDZERO` = the 50 × 18 canvas, an
  `osd_layout` for 50 × 18 with the horizon on); the resolver picks them by the board's
  `firmware_family` (`board.json`; `rotorflight` when absent). The RS7 assemblies are unchanged.
- **The VTX has to talk**: iNav pushes DisplayPort frames only while the VTX sends it MSP requests
  (`vtxActive`, 1 s timeout). The device now sends MSP_FC_VARIANT every 125 ms from power-on (timer
  `osd.poll`; one request in flight; not while the power is off or `link_lost` is on) and keeps the
  answer (`INAV`/`RTFL`) → `font` in the `osd` message. Only on the speedybee-f405-wing by default
  (`FbBoard.osd_poll`, `-M osd-poll=on|off`): RotorFlight needs no poll, answers every request
  outside boot/reboot/CLI mode, but the requests shift its scheduling (RS7-osd FDM log `1e950839aa87`
  with them, `233f9d6f4ec7` without = main's), so the nexus-xr leaves it off. iNav answers the canvas request (188) with `$M!` (counted).
- **What iNav sends** (the stream recorded with `-M osd-log=`): MSP v1 `$M>` only, no jumbo; HEARTBEAT
  with every OSD refresh, OPTIONS `[0][1]` (HD 50 × 18) with every clear, CLEAR once a second
  (`osd_msp_displayport_fullframe_interval`) followed by every non-blank cell, otherwise only the
  changed cells, DRAW whenever something was written (up to 62.5 Hz; ~47/s with the horizon). attr
  = the font page in bits 0..1 (0 or 1), no blink, no version bit; no RELEASE, no SYS. The parser
  took all of it as it was; OPTIONS is now decoded (`fb_msp_osd_options`), the rest was already
  firmware-neutral. Replay tests: `devices/tests/test_msp_osd.c` feeds `evidence/inav-osd/stream-*.bin`.
- **Bench line**: `osd_polls`, `osd_variants`, `osd_errors`, `osd_options`, `osd_mode`, `osd_fc`,
  `max7456_display_writes`, `max7456_clears` (the board's chip stays at 0 with the MSP OSD).
- **The app**: iNav's codes index iNav's 512-glyph font (glyph = `(attr & 3) << 8 | byte`), not the
  RotorFlight atlas; see the evidence README's app note.

### Wiring model (branch `wiring-model`, 2026-09-26)
Three inputs, each knowing only its own side, one resolver (`scripts/wiring.py`):
- **board** = pads + onboard devices. Firmware id, kind, pins, timer, DMA, bus come from the unified
  config (`config_pads()`: `gyroN` GYRO_CS/EXTI + `gyro_N_spibus`, `i2cN`, `uartN` TX/RX (RX-only →
  flag `rx_only`), `motorN`/`servoN` with `timer`/`dma` from the config's comment lines, `freqN` = RPM
  capture inputs, `flashN`); `boards/<id>/board.json` adds what the config does not have: the printed
  label (`null` where not confirmed), extra flags (`5v`, `usb`), extra pads (`usb`) and the onboard
  devices (`pad`, `device`, `detachable`, `part`). Nexus-XR: 16 pads, 4 onboard (ICM-42688-P on gyro1
  detachable, SPL06 baro on i2c3 detachable (soldered on the real board; since branch `baro`), ELRS
  receiver on uart5 detachable, W25N02 flash soldered).
- **devices** = the C descriptors with `ports` (`id`, pad `kind`, `dir` from the device's side,
  `optional`, `bundled`): ESC `signal` motor in + `tlm` uart out + `rpm` capture out, IMU `spi` + `int`
  (bundled: comes with the SPI pad), servo `pwm`, receiver `uart`, ...
- **assembly** = the session's `wiring`, names only (`docs/protocol.md`): new form
  `{"device","port","pad"}` (pad by firmware id or label) and `{"device","detached":true}`, complete
  and strict; the old `{"device","slot"}` form (what the app writes) keeps the pre-wiring-model
  behaviour: receiver (uart5 unless it names another modelled UART), ESC and servo1..4 assumed
  present (logged once: `old-form session: receiver/ESC/servos assumed present (...)`), an omitted
  gyro is off as `imu=off` was, an entry that cannot be placed is a warning. A list mixing both forms
  is rejected.

The five consumers read the one resolved table:
| consumer | reads | from |
|---|---|---|
| manifest (`manifest.py`) | `boards[].pads` (id, label, kind, flags, pins, timer, dma, bus, onboard/device/detachable, `devices` = what the machine models there) + the unchanged `slots`; `devices[].ports` + the old `kind` | `load_board()`, `slots_of()`, `device_catalog()`, `MODELLED` |
| session check (`session.py`) | new form: unknown device/port/pad, port kind ≠ pad kind, UART direction vs the pad's TX/RX, one device per pad (an I2C bus takes several, not two of one device = same address), removing a soldered onboard device → error; both forms: a prop not in the device's descriptor or out of its type/range (the app's empty `actuator:""`, `reversed:false`, `option:""` tolerated), a servo `actuator` not one of the aircraft's `fdm_inputs`, an ESC `actuator` other than its `throttle`, `actuator` on another device, an actuator driven twice, mixed forms, a bad `aircraft.json` → error; old form: an entry that cannot be placed → warning; valid but unmodelled → warning, left out | `resolve()`, `load_aircraft()` |
| device instantiation (machine) | `-M imu=on\|off` (off: SPI1's CS and the INT1 line go nowhere), `crsf=uart3..6\|off` (USART1 = MSP host, USART2 = RX-only TELEM), `esc=on\|off`, `baro=on\|off` (off: 0x76 NACKs), `servos=<mask>`, `osd=uart3..6` (the MSP DisplayPort OSD device, only when wired; its CLI lines go into the custom defaults): a device exists only when the assembly has it; defaults (no session) = the RS7 layout | `machine_options()` |
| actuator map into the FDM | `actuators=` (names per output servo1..4, motor1), `servo-rev=`; the machine maps FDM input k (aircraft file `fdm_inputs`) to the wired output with that name, mirrored around the input's `centre_us` (aircraft file, default 1500: `2·centre − µs`) when reversed, 1500 when none; an unnamed servo output takes the FDM input of its position, the ESC's output is always the aircraft's `throttle`; `state.actuators`/`pwm_us` are `null` for an output with nothing wired | `machine_options()` + `aircraft-file=` |
| faults (`ctl_dev_wired`) | a fault is accepted only for a device the session wired (`crsf`: receiver on a USART, `esc`, `icm42688p`, `dps310`), else `fault: device X is not wired in this session`; `state.fc.link` is down without a receiver | the same `-M` options |

RS7-specific numbers left the machine: gear, ESC (kv, poles, τ, currents, pack voltage) and FDM
parameters are in `models/rs7/aircraft.json`, loaded through `-M aircraft-file=` (the session passes
it; `run.sh` without a session passes `AIRCRAFT_FILE=models/rs7/aircraft.json`). Without a file the
machine uses the libraries' defaults and a 1:1 gear. The file is checked twice, with the same rules
(`wiring.load_aircraft()` before start, `aircraft_load()` in the machine for a direct `-M`): a wrong
type or value exits naming the key (`esc.kv` > 0, `esc.poles` even 2..64, gear ratio = 2 numbers
> 0, `centre_us` integers 1000..2000, the other ESC/FDM numbers positive or non-negative), and a heli
FDM without `transmission.main_rotor_gear_ratio` is an error, not a silent 1:1.

Evidence `evidence/wiring-2026-09-26.log` (`scripts/wiring_evidence.sh`, base = `main` 2ca57c8 built
in the same tree):
| # | check | result |
|---|---|---|
| 1 manifest | `run.sh --manifest`: valid JSON, 16 pads (labels/kinds/flags/pins/timer/DMA/onboard), 8 devices with `ports`; old `slots` (13) and `kind` kept; `test_wiring.py` checks the pad table against the 13 slots of the app's rule (16 tests OK), `devices/` tests OK | pass |
| 2 session | `rs7.json` (old form, unchanged) and `rs7-pads.json` (new form, TELEM/RPM/TAIL by label) resolve to the same machine options; rejected: unknown pad `S9`, servo.pwm on PORT-A, removing a soldered onboard device (the blackbox flash; the baro was soldered in this run and is detachable since branch `baro`: `{"device":"dps310","detached":true}` → `baro=off`), gyro removal on a board.json without `detachable`, servo1 used twice, GPS on the onboard receiver's uart5, unknown device, receiver on RX-only TELEM; accepted: compass next to the baro on i2c3, gyro removal on the real board.json (`imu=off`) | pass |
| 3 flight | base build: lift-off 33.82 s, head speed and governor 1793..1802; this build, `rs7.json`: 33.82 s, 1793..1802; `rs7-pads.json`: 33.82 s, 1793..1802. Quiet runs: FDM log md5 28450cfb7f01 (3101 rows to t = 62 s) for `rs7.json`, `rs7-pads.json` and the base build — bit-identical | pass |
| 4 reversed servo2 | roll stick 1800, disarmed: outputs [1500, 2000, 1001]; normal → FDM inputs [1500, 2000, 1001] (swash deflection 0/+500/−499: roll), servo2 reversed → FDM inputs [1500, 1001, 1001] (0/−499/−499: the roll turns into collective/pitch); `state.actuators.swash_b` +1 vs −0.998 | pass |
| 5 no receiver / no ESC | `crsf=off`: RXLOSS at every sample t = 6..30 s, arm switch ignored, receiver telemetry frames 0, `fc.link` down; `fault crsf` → `device crsf is not wired in this session`. `esc=off`: armed with throttle on, `dshot` 0, `frames=0`, motor/head rpm 0; `fault esc.telemetry_lost` → not wired | pass |
| 6 ctl / fault evidence | `ctl_evidence.py` all stages ran as before (MSP_RC, link/failsafe, pause/step, realtime, reset/firmware/power/watch, CLI); `fault_evidence.py` SUMMARY 1a..3 all PASS | pass |
| 7 review fixes (`## review fixes`) | 34 unit tests OK (props, actuator roles, mixed forms, old-form defaults, I2C address clash, aircraft.json checks, config without `# pin`); flight: lift-off 33.82 s for base/`rs7.json`/`rs7-pads.json`, quiet FDM log md5 28450cfb7f01 for all three (bit-identical); reversed servo2 → FDM input 1000 for output 2000; no receiver (new form): RXLOSS throughout, 0 frames; old form without crsf → `crsf=uart5` + the `assumed present` line; the app's smoke bench (gyro only, old form) starts with receiver/ESC/servos; unwired servo3 and no ESC → `null` in `actuators`/`pwm_us`; IMU detached → 0 samples, ODR 0, `NOGYRO`, SPI:0; the machine alone rejects `crsf=uart2`, a heli aircraft file without transmission, `kv:"520"`, `poles:7`, `centre_us` 2600; ctl/fault/ground evidence all PASS | pass |

### Flight evidence (milestone 2 step B, 2026-09-25; `scripts/flight_evidence.sh`)
`evidence/flight-2026-09-25.log` (pilot/MSP log + report), `evidence/flight-fdm-2026-09-25.csv` (FDM truth
every 20 ms), `evidence/flight-2026-09-25.csv` (MSP samples joined with the FDM row nearest in virtual time:
t, alt, vz, head rpm, FDM roll/pitch/yaw, servo 1..4, collective, on_ground, armed, MSP roll/pitch/yaw,
governor target/current). Plan `liftoff`: ARM (ch5) at 14 s, throttle switch (ch6) at 15 s, collective ramp
32–34 s to 1720 µs then 1640 µs (the model hovers at about 1625: 3° of 12 at full stick), roll/pitch/yaw doublets (±100 µs, 0.3 s) at 40/43/46 s.

| what | value |
|---|---|
| arming | ARMED at 14 s with no arming-disable flags; BOOT_GRACE cleared by itself between 4.9 and 6.9 s with the live CRSF receiver |
| governor (ELECTRIC, 1800) | spools 15 → ~23 s; airborne target 1800, current 1793–1802 rpm (MSP_DEBUG, `debug_mode = GOVERNOR`), head speed from `fb_esc` via bidirectional DShot |
| lift-off | t = 33.82 s; airborne ≥ 30.7 virtual s, never touched down; slow climb to 3.7 m (0.07 m/s at 1606 µs) |
| attitude, hands off | MSP_ATTITUDE \|roll\| ≤ 1.0°, \|pitch\| ≤ 0.4° (doublet windows excluded) |
| attitude incl. doublets | FDM \|roll\| ≤ 8.4°, \|pitch\| ≤ 9.1°, each doublet damped in < 1 s |
| firmware vs FDM attitude | \|Δroll\| ≤ 1.0°, \|Δpitch\| ≤ 0.9° (MSP pitch is + nose down, FDM + nose up) |
| determinism | two runs gave the same FDM maxima (8.36° / 9.14°) |

Entering the CLI while armed is not possible: the firmware skips non-MSP bytes on MSP ports when armed
(`fc/tasks.c`), so the armed state is read over MSP_STATUS.

### Control server (step C, `hw/arm/nexus_xr_ctl.c.inc`)
The protocol of `docs/protocol.md` on TCP 31250, inside the machine (QEMU's own event loop; no sidecar,
no second clock). A `QIONetListener` accepts any number of clients; their commands are handled in the
main loop. Everything periodic hangs off the 1 kHz physics tick in virtual time: `state` (100 Hz by
default, per client `subscribe`), `samples`, the MSP poller, the realtime throttle. A client that does
not read gets its `state`/`samples` dropped (backlog > 64 KiB), never blocks the machine.
- **USART1 has two masters.** The core polls MSP_STATUS and MSP_ATTITUDE (plus FC_VARIANT/FC_VERSION/
  BOARD_INFO/BOXNAMES once per boot) through the same paced wire as the host socket :31235, one
  request in flight. The host → FC stream is followed frame by frame (`$M<`/`$X<` header, length,
  payload, checksum): the core's request goes into the FIFO only between complete host frames (a
  stalled host frame is abandoned after 100 ms), and a `#` from the host counts as a CLI entry only
  between frames. The firmware's replies to the core's requests are parsed out of the TX stream
  (v1 checksum checked) and not forwarded; a request that timed out (200 ms) is remembered for 2 s
  so its late reply is swallowed too; everything else (other MSP frames whole, text byte by byte)
  goes to the host. Output to the host, pilot and motion sockets is non-blocking: a bounded ring
  (64/16/16 KiB) flushed on G_IO_OUT, overflow dropped with a counted log line. Checked: MSP_RC read
  over :31235 by a script while the core polls.
- **CLI**: `cli` sends `#`, waits for the banner, then the lines; FC text becomes `log` lines; CLI mode
  ends at the next MCU reset (`exit`, `save`, reset, power).
- **Time**: `pause` = `vm_stop`, `step` = `vm_start` + a virtual-clock timer that requests the stop.
  Realtime: the physics tick (a virtual-clock timer, BQL held, under icount on the vCPU thread) never
  sleeps; when virtual time is ahead of host time × speed it queues a sleep on the vCPU with
  `async_run_on_cpu`, which waits with the BQL released (`qemu_cond_timedwait_bql`, the pattern of
  QEMU's own cpu-throttle), so the main loop keeps serving clients. Under icount the vCPU's
  instruction count is the virtual clock: a sleeping vCPU is stopped virtual time, so "never faster
  than wall clock × speed" holds per tick, and `timing.realtime_factor` measures the result. (`-icount
  sleep=on,align=on` was not used: no speed factor, not switchable at runtime. Without icount nothing
  is throttled; a `log` line says so.)
- **Resets** go through QEMU's system reset; the machine's reset handler installs a pending firmware
  image or erases the config sector before the CPU fetches its vectors. Power off = reset + the CPU
  switched off (`arm_set_cpu_off`) by a reset handler registered after the CPU's; power on = reset
  with power-on flags, the IMU reset too.
- **Beeper**: `beeper=<pin>[:inv]` from `resource BEEPER`; the Nexus-XR has none, so on this board
  `motor_beep` from DShot beacon commands (value 1..5 in a decoded frame) is the only sound source.
- **Manifest**: built by the machine from the bench (`-M session=`, in memory), or the machine option
  `manifest=<file>` (bare `run.sh <elf>` passes the output of `scripts/manifest.py`; it also overrides
  the bench's); the server sends it as one line right after `hello`, before `signals`. Without either
  no `manifest` is sent.
- **Faults**: `fault` is checked against the device descriptors (`devices/include/fb/device.h`,
  compiled into the machine with the models) and applied to the models the machine has wired in
  (`crsf`, `icm42688p`, `esc`: `fb_<dev>_set_fault`). The active set is re-read every physics tick;
  each change becomes one `fault` event, so `link` and the pilot socket's `link` field (aliases of
  `crsf.link_lost`) report too. `state.faults` lists them. Faults are kept across MCU resets.
  `esc.telemetry_lost` makes the machine skip the bidir GCR reply (`esc_reply`).
- **MSP_RC**: the poller cycles MSP_STATUS / MSP_ATTITUDE / MSP_RC, one request per 33 ms (~10 Hz
  each), `fc.rc_us` = the channels the firmware decoded.

Evidence (2026-09-25, `evidence/ctl-2026-09-25.log` from `scripts/ctl_evidence.py`, the core's stderr in
`evidence/ctl-core-2026-09-25.log`, 1 s of messages in `evidence/ctl-dump-2026-09-25.jsonl`; the app's
headless run in `evidence/ctl-app-smoke-2026-09-25.log`, realtime at `icount shift=3` in
`evidence/ctl-realtime-shift3-2026-09-25.log`):

| stage | what was checked | result (virtual t) |
|---|---|---|
| 1 hello + state | `hello` (board, firmware from MSP or file name, aircraft, actuators, state_hz), `signals` (80 ids), `state` | 200 states per 2.00 virtual s; 1 s dump: 101 states t=2.35..3.35 (`ctl-dump-*.jsonl`); `fc` from the core's own MSP poll: `cpu_pct` 8, arming flags, mode, attitude |
| 1 slow client | a client that connects and never reads, 8 host s | 0.380× vs 0.388–0.396× without it: the machine does not wait |
| 1 app | `FB_BENCH=smoke FB_CORE=127.0.0.1:31250` Godot 4.7.2 mono headless `--quit-after 400` on the homelab | `103 states, last t = 107.70 s` (`ctl-app-smoke-*.log`) |
| 2 rc | `rc` 1100,1200,1300,1400,1000,1000,1700,1800 → MSP_RC read over :31235 while the core polls | 1100,1200,1400,1300,1000,1000,1700,1800 (firmware order R,P,Y,C,T,AUX: map AECR1T23 swaps ch3/ch4) |
| 2 link | `link up:false` / up with rssi −85, lq 70 | `RXLOSS` flag, `failsafe {on:true}` at +0.19 s; cleared, `failsafe {on:false}`; `fc.link` follows |
| 3 time | pause 2 host s; 3 × step 0.01; run | t frozen, 0 states; 24.337 → .347 → .357 → .367; runs again |
| 3 realtime | `realtime:true` speed 1 / 0.25 / 0.1, off | 0.388× (machine slower than wall clock: nothing to throttle) / 0.250× / 0.100× / 0.396×; at `icount shift=3` the full loop is 0.59× — 1.0× is not reachable on this host with gyro + FDM (`ctl-realtime-shift3-*.log`) |
| 4 reset aircraft | armed via `rc` (t=33.5), governor 1799 rpm, collective ramp, airborne (0.51 m at 47.88, 0.76 m at 48.88), then `reset aircraft` | at 48.93 pos/vel 0, `on_ground:true`; `disarmed` event when the sticks go back |
| 4 reset attitude | airborne, roll stick until the FDM is at 73.5° roll (firmware 68.1°), then `reset aircraft`; then the same with `firmware:false`, still armed | FDM on the pad, `reboot` at once, firmware attitude [0,0] at its first MSP answer 2.32 s after the reboot, re-arms after the switch is cycled; with `firmware:false` no reboot and the firmware still says 24.3° roll 5 s later (`reset-attitude-2026-09-25.log`, `scripts/reset_attitude_evidence.py`) |
| 4 ground contact (branch `fdm-ground`; `reset aircraft` default is teleport-only again) | yaw to ~84° with the rudder, collective eased down; take off, full roll stick + collective 1350 into the ground; then `reset aircraft` (no `firmware`) while armed | touchdown at 0.49 m/s sink, 1.19 m/s drift: FDM yaw 83.6 → 83.6, firmware 83.0 → 83.0, no `crash`. `crash {roll_deg:128.5, pitch_deg:8.0, speed_mps:7.43}` once; FDM upright at +0.26 s, firmware roll 15.6 at +0.26, 2.1 at +0.36, |fw−FDM| 1.8° at +1.0 s, heading 104.4 kept, no reboot. Reset: FDM yaw 104.4 → 0 in 0.25 s, firmware yaw 104 → 13 at +0.26 s, 0 at +0.51 s, no `reboot`, still armed; disarm/arm works (`ground-2026-09-25.log`, `scripts/ground_evidence.py`) |
| 4 reset firmware | `set name = BENCH` + `save` before, then reset | `reboot {cause:reset}`, CLI `get name` → `BENCH` (flash kept), `status` ok |
| 4 power | off 2 virtual s, then on | virtual time runs on, `fc.power:false`, DShot 0, no MSP reply on :31235; on → `reboot {cause:power-on}`, `name = BENCH` still there |
| 4 reset all | | two reboots (config erased → custom defaults → saved → reset), `name = -` |
| 4 firmware | `.hex` of the same build; a nonexistent path | `firmware_loaded`, power-on reboot, MSP: `rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)`; `error` "Failed to open file" and the running firmware stays |
| 4 watch | `watch:true` on a copy of the ELF, `touch` | "changed on disk, reloading" (1 s mtime poll, reload once the change held still for one poll), `firmware_loaded` + reboots, MSP answers again |
| 5 cli | `status`, `beacon RX_SET`, `aux 5 13 2 1700 2100 0`, `set name`, `save`, `get name`, `exit` | replies as `log src:cli`; `fc.cli:true` in CLI, false after `exit`/`save` (both reboot) |
| 5 events | | `armed`, `disarmed`, `failsafe`, `reboot` (cause), `power`, `reset`, `firmware_loaded`; BEEPER switch → `motor_beep {cmd:1}` every 0.2–0.4 s (DShot beacon); no `beep`: Nexus-XR has no BEEPER resource |
| 5 signals | `subscribe` 20 Hz with 5 ids (+1 unknown) | `error` for the unknown id; 20 `samples` per virtual s with the other 5 |
| 6 session | every run above: `run.sh --session sessions/rs7.json`; the shift-3 run through `FB_SESSION=` | wiring checked (`dps310 -> i2c3: not modelled, left out`), actuators, icount, ports, realtime from the JSON |

### Manifest and fault injection (branch `manifest`, 2026-09-25)
`evidence/manifest-faults-2026-09-25.log` from `scripts/fault_evidence.py --port 31264 --msp 31260 --mon 31261`
against its own core (`sessions/rs7.json` with ports 31264/31260, HMP 31261), the core's stderr in
`evidence/manifest-faults-core-2026-09-25.log`. MSP_DEBUG/MOTOR_TELEMETRY/ANALOG are read by the script over
the MSP socket while the core polls.

| check | what | result (virtual t) |
|---|---|---|
| 1a manifest | `run.sh --manifest` (with `QEMU=/nonexistent`: nothing starts) vs the message after `hello` | valid JSON, equal as parsed JSON, order `hello`, `manifest`, `signals` |
| 1b slots | the app wizard's regexes (`WiringGraph.cs`) on `NEXUS_XR.config`, independently in the script | 13 = 13: gyro1 spi (icm42688p), i2c3 i2c, uart1..uart6 uart (uart5: crsf), motor1 motor (esc), servo1..4 servo (servo); all 7 slots of `session.py` among them |
| 1c devices | faults per device, `implemented`, aircraft | crsf link_lost/freeze/lq (lq params lq, rssi_dbm, snr_db), icm42688p freeze, esc telemetry_lost; dps310, gps, compass `implemented:false`; rs7 `fdm:heli`, actuators swash_a, swash_b, swash_c, tail, motor |
| 1d errors | unknown device, unknown kind, dps310 (no faults), lq without / with lq 101 | 5 `error`s, `state.faults` stays `[]`, no `fault` event |
| 2a crsf.freeze | hover at 2.3 m, freeze, pilot roll 1500 → 1580 for 1.5 s, collective loop keeps moving ch3 | firmware MSP_RC roll 1500 and collective 1605 constant over 1.69 s (170 states), armed, no RXLOSS/FAILSAFE, no `failsafe` event; off → firmware sees roll 1540 0.09 s later |
| 2b crsf.lq | lq 30, rssi −95 for 4 s, then off | `fc.link` {lq 30, rssi −95, up}; firmware RSSI (MSP_ANALOG, 16-frame average) 550 → 498, 464, 412, 378, 326, 292, 275, 275 every 0.5 s (−95 dBm = 275 on the −130..0 scale), back to 550 in 3.5 s after off |
| 3 crsf.lq decoded | on the ground, `debug_mode = CRSF_LINK_STATISTICS_UPLINK` (CLI, save) | MSP_DEBUG LQ 100 → 30 → 100; debug_mode saved back to GOVERNOR |
| 2c esc.telemetry_lost | 3 s in the hover | the machine's bidir reply counter stops (76784 before and after); ESC model 16054 → 16343 rpm, firmware MSP_MOTOR_TELEMETRY rpm 16040 and governor current head speed (MSP_DEBUG[2]) 1797 frozen, invalid-telemetry percent 0: RotorFlight updates the RPM only when reply edges arrive, so it regulates on the last value and the head speed drifted 1799 → 1832; back → firmware 16080 vs model 16054 within 0.5 s |
| 2d crsf.link_lost | in the hover | `failsafe {rxloss:true}` +0.135 s, `fc.link` up false LQ 0; the firmware cuts the motor (head 1799 → 163 rpm in 1.25 s), the model falls 2.3 m, `crash` at the ground; off → `failsafe` off +0.015 s, the governor spools up again (901 rpm 4 s later) |
| 2e icm42688p.freeze | after `reset aircraft`, lift off again, hover; freeze 1 s with roll stick 1560 | firmware attitude 0.1° change while the FDM rolled 65.6° (the controller sees no rate response and keeps rolling); after off the firmware's attitude is still ~60° off (3 s later FDM roll 59.3°, firmware −4.9°), `crash {roll_deg:82.2, speed_mps:23.44}` |

### Review 1 fixes (branch `review-core-1`, 2026-09-26)
`evidence/review1-2026-09-26.log`: unit tests, `ctl_evidence.py`, the lift-off flight, the realtime
check and the USART1 check, each on its own core (ports 31260..31264, a separate QEMU build).

| check | result |
|---|---|
| `make -C fdm test`, `make -C devices test` | green, incl. new `test_json`, `test_fw_image` (also clean under ASan/UBSan), servo delay/crash thresholds, ESC signal loss, ICM soft reset in a transaction, eRPM edges across a 16-bit wrap |
| `ctl_evidence.py` | all stages as before (rc, link/failsafe, pause/step, realtime 0.248×/0.100×, reset/power/firmware/watch, CLI `get name = BENCH`), bad firmware path → `error` |
| lift-off (`flight_evidence.sh` plan, same options) | lift-off t = 33.82 s (as on 2026-09-25), airborne to the end, governor 1793–1802 rpm, FDM \|roll\| ≤ 8.37°, \|pitch\| ≤ 9.17° (8.36/9.14 before: the servo delay is now 8 ms instead of 7), \|MSP − FDM\| ≤ 1.0/0.9° |
| realtime, icount shift=3 | speed 0.5/0.25/0.1 → 0.495/0.251/0.100×; speed 1 (machine at ~0.55×) no penalty (0.549/0.550× vs 0.544/0.548× off); ping → pong 0.2 ms median, ≤ 2.4 ms max while throttled |
| non-reading pilot client | 500k commands, replies never read: the core keeps 0.552×, drop logged (`pilot socket: ... dropping`) |
| 0x23 inside host MSP frames | `rc` → `fc.rc_us` 0.110 s (main before: 1.080 s, the poller paused for a CLI entry) |
| host streaming MSP requests | the core's own poll goes on between the host frames: `rc` → `fc.rc_us` 0.07–0.10 s (main before: never, the 20 ms quiet rule starved it); host replies all MSP_STATUS with good checksums |

### Review 2 fixes (branch `review-core-2`, 2026-09-26)
`evidence/review2-2026-09-26.log`: unit tests, `ctl_evidence.py` (all stages), `cli_msp_evidence.py`,
`uart_evidence.py`, the core's stderr; own core on ports 31270..31274, a separate QEMU build.

| check | result |
|---|---|
| `make -C devices test`, `make -C fdm test` | green, incl. HEX without data records refused, `fb_fw_erased`, FDM: a tipped model hopping 12× (≤ 0.15 s each, all > 15°) is 1 crash, a real flight re-arms |
| core polls as MSP v2 | the firmware answers v2 for VARIANT/VERSION/BOARD_INFO/BOXNAMES/STATUS/ATTITUDE/RC on the first try; `fc` filled as before |
| `ctl_evidence.py` | all stages as before; refused `subscribe` (unknown id, `log:"false"`) leaves 100 Hz state and no samples; `power`/`fault` with string/number `on`, `params` not an object, a string param, an unknown param → `error`, nothing applied; the ELF, HEX and watched-copy loads pass the erased-area check for the defaults blob |
| CLI while a host streams MSP v1 | 3 rounds `cli status` → 16 lines each (banner + output), 297 host requests, 297 valid replies, 0 bad, 0 core v2 replies leaked |
| `uart_evidence.py` | 0x23 in host frames: `rc` → `fc.rc_us` 0.110 s; host streaming MSP_STATUS: 51/51 good replies, poll goes on (0.08–0.10 s) |

### Idle skip, "external WFI" (branch `idle-skip`, 2026-09-26)
RotorFlight never executes `WFI`: between tasks `run()` calls `scheduler()` again and again, which
reads **DWT CYCCNT** (`getCycleCounter()`, lines 486/616/678 of `scheduler.c`; our `dwt_read`) and
**SysTick VAL** (`micros()`, `drivers/system.c`, QEMU's own systick, plus `SCB->ICSR`) and walks the
task checkers. Under `-icount` every one of those instructions costs host time. `-M idle-skip=on`
(session `time.idle_skip: true`, optional `idle_skip_n`, `idle_skip_gran_us`) fast-forwards them
without touching the firmware:
- **Detection** (in the CYCCNT read handler): each read gets a signature = PC of the translation
  block doing the read + guest instructions executed since the previous read (exact under icount).
  An idle `scheduler()` pass is a fixed cycle of p ≥ 2 signatures (here p = 3: 463 + 48 + 21
  instructions, `bench` shows it as `idle_cycle`). Idle = `idle-skip-n` (16) consecutive reads each
  match the read p before them (period p = 2..8, so ~n + p reads and at least two cycles), the CPU
  is in thread mode with PRIMASK/BASEPRI/FAULTMASK clear, and in between there was no NVIC interrupt
  (the NVIC → CPU line is intercepted) and no access to a *reporting* peripheral. Single-read spins
  (p = 1, the ≤ 12 µs wait for the gyro target) are left alone: their end is no event the machine
  could stop at.
- **Which accesses break an idle cycle.** Reporting (handler calls `idle_touch`): GPIO, EXTI, SPI1,
  DMA2, the timers, the stub windows (APB/AHB1, AHB2, FMC: RCC, flash controller, other SPIs, …),
  flash writes. **Not reporting**: the 8 QEMU USARTs (`stm32l4x5_usart`), the armv7m PPB (NVIC, SCB
  incl. ICSR, SysTick, MPU, the PPB's RAZ/WI default region), DBGMCU, DWT writes and CTRL/LSR reads,
  flash/ITCM/SRAM reads. The PPB is part of the idle pass itself (`micros()` reads SysTick VAL and
  ICSR); whatever it changes that matters reaches the CPU as an NVIC exception (intercepted) or a
  virtual timer (SysTick, a deadline the skip stops at). USART polling needs no reporting: TX
  completes inside the TDR write (every USART is on an in-process fbwire chardev, which always
  accepts, or on none: TXE/TC are set before the next read), RX bytes come from the main loop and
  set RXNE together with the IRQ the firmware drains them by (an NVIC edge = not idle); a pure flag
  spin has no CYCCNT read, and a spin with a CYCCNT timeout loses at most 50 µs per cycle to a skip,
  time that would pass anyway.
- **Skip** (`idle-skip-mode=budget`, default): the skipped instructions are taken out of the vCPU's
  icount budget at once, as if the poll had executed. The length comes from the **virtual clock
  only**, read at the skipping CYCCNT read: `MIN(idle-skip-gran-us (50 µs), next QEMU_CLOCK_VIRTUAL
  deadline)`, and the budget is extended when it is shorter. The leftover budget is not used as the
  length, because QEMU also cuts it at the next `QEMU_CLOCK_REALTIME` timer (monitor/QMP rate
  limiter, chardev reconnect) — then how far a skip jumps, and the time the firmware reads, would
  depend on host state. Reading the deadline at the skip also covers a virtual timer armed from the
  main loop while the vCPU runs (ctl `step`, the host UART inject pump), which the budget computed
  before it would overshoot. No timer and no interrupt is jumped over: the vCPU leaves `cpu_exec`,
  runs the due timers on its own thread and continues; the read that triggered the skip already
  returns the advanced time. After a skip or an interrupt, one full idle cycle (p matching
  reads) after that fresh read is needed before the next skip, so a task that became due by time
  always gets its pass (at most 50 µs late). An exception pending at the CPU prevents the skip
  (`CPU_INTERRUPT_HARD|NMI` only: `cpu_has_work()` also looks at `EXITTB`, which other threads raise
  at host-timed moments, and with it two identical runs diverged).
- **`idle-skip-mode=halt`** does what the task first asked for: `cs->halted = 1` + `cpu_exit` (the
  state `HELPER(wfi)` sets), NVIC interrupts wake it through `cpu_has_work` like WFI, a virtual timer
  after 50 µs at the latest. Same skipped time, but under icount the warp to the next deadline is done
  by the **main loop** (`icount_start_warp_timer` after `qemu_notify_event`), so every halt is a
  vCPU → main loop → vCPU round trip; at ~23 000 halts per virtual second that costs more than the
  budget mode (19.5 vs 15.3 host s below). Kept for comparison only.
- Counters: `qom-get /machine bench` → `idle_skips`, `idle_skipped_s`, `idle_extended` (skips that had
  to extend a budget cut short by a realtime deadline or by a virtual timer deleted/moved later from
  the main loop), `idle_cycle`;
  `state.timing.idle_skip`, `idle_skip_pct` (share of virtual time skipped over the last ~1 s).
  Without `-icount`, with `-icount shift=auto` (adaptive) or with record/replay the option is
  switched off with a warning. `idle-skip`, `idle-skip-mode`, `idle-skip-gran-us` are read once at
  machine init (NVIC interception, granularity in instructions): set them with `-M` only, a later
  `qom-set` is refused. `idle-skip-n` is read at every CYCCNT read and may be changed any time.
- Step check: when a ctl `time` step stops more than one instruction past its boundary, the core logs
  `time: step boundary … overshot by N ns (reason)`: `an idle skip jumped over it` (must not happen)
  or `armed while running` (QEMU's icount race between the main loop arming the timer and the running
  vCPU budget, also with the skip off).

**iNav: the SysTick/ICSR pattern** (`idle-skip-src=systick`, the speedybee-f405-wing default;
`evidence/speed-inav/README.md`). iNav's idle loop is `main()` → `scheduler()` → `micros()` again and
again; `micros()` (`drivers/time.c`) reads `SCB->ICSR` (VECTACTIVE) and `BASEPRI`, then `sysTickUptime`,
`SysTick->VAL`, `sysTickUptime` again in a retry loop (and `sysTickValStamp`), no CYCCNT. One idle pass
calls it 4 times: at the top of `scheduler()`, for the RX task's `checkFunc`, and before/after
`taskRunRealtimeCallbacks()`; the detector sees a cycle of p = 4 VAL reads (63, 60, 154, 504
instructions apart on the trainer), locks after the usual `idle-skip-n` matching reads and then skips
at the end of every idle pass: ~3 µs of executed pass, 50 µs skipped, and so on until a task is due.
Once the scheduler runs, the bare F405 board costs 0.41 host s per virtual s (1.9 with the skip off). It does **not**
skip `delay()`/`delayMicroseconds()`: those are single-read spins (`while (micros() - now < us)`,
p = 1) whose end is a time only the firmware knows, and the boot is made of them (the LED/beeper
flash, `delay(500)` and a 5 × 100 ms flash on a cold boot, the USB disconnect, the gyro's init: ~7 M
ICSR + 7 M VAL reads per virtual s, the boot runs at 0.11–0.16×). Skipping them would move where every
delay ends, i.e. the guest's timing. Phase 1's figures (1.25× over virtual 10..20, the NVIC/SysTick
reads "drop only ~7 %" over 0..20) are windows that hold such boots; the steady state is skipped as on
RotorFlight.
The signature's PC is `env->regs[15]` at the read: under TCG the start PC of the TB that does the
read (QEMU stores the PC at TB exits and rewinds, not per instruction). With a device access in the
middle of a TB, `cpu_io_recompile()` rewinds to it and it is the reading instruction's PC; with the
read as the last instruction of a TB cut by the icount budget, it is that TB's start. So the idle
skip's decisions depend on how QEMU cuts TBs: anything that changes TB shapes (`io-tb=on`) changes
them, deterministically, and with them the guest's timing.

`evidence/idle-skip-2026-09-26.log` (`scripts/idle_skip_evidence.sh`, own core on 31270..31274, homelab
i7-4790 with the user's core running next to it; RS7 session, `icount shift=2`):

| check | skip off | skip on |
|---|---|---|
| A idle on the pad, disarmed (speed.py, host s / 10 virtual s) | 27.4, 27.5 | **15.3, 15.2** (77 % of virtual time skipped, 29 200 skips/virtual s); halt mode 19.5 |
| A bare firmware, NOGYRO, no FDM/control server | 22.5 | **2.5** (95 % skipped) |
| B in flight, virtual 40..62 s (flight.py --quiet) | 28.1 | **15.4** (76 % skipped) |
| C CLI `tasks` / `status` on the pad | GYRO 4000, FILTER 2000, PID 4000, ACC 1000, ATTITUDE 500, RX 251 Hz; max/avg GYRO 8/2, PID 28/16 µs; `CPU:8%`, cycle time 250, total 8.3 %; arming flags only `CLI` | the same rates (RX 250, TELEMETRY 991 vs 1000 Hz), max/avg GYRO 8/1, PID 28/16 µs (none higher), `late` counts 0–9 as off, `CPU:8%`, cycle time 250, total 7.7 %; arming flags only `CLI` |
| D lift-off plan with MSP polling | lift-off 33.82 s, FDM \|roll\| ≤ 8.37°, \|pitch\| ≤ 9.17°, governor 1793–1802, hands-off MSP ≤ 1.0/0.4° | two runs: the same numbers |
| D determinism, FDM log every 20 ms to 62 s, no host polling | two runs: FDM log identical (20 ms sampling, printed precision) | two runs: FDM log identical (20 ms sampling, printed precision), **and identical to skip off** (md5 28450cfb7f01) |
| D determinism with a QMP client polling `qom-get /machine bench` every 100 ms (review fixes) | | two runs: FDM log identical, and identical to the no-client runs (see the log's "review fixes") |
| D ground_evidence.py | (ground-2026-09-25.log) | 2 runs, all 4 checks PASS; crash 128.3°/7.42 m/s (128.5/7.43 before: host-timed pilot) |
| E realtime, speed 1 / 0.5 / 0.25 / 0.1 | | shift 2 and 3 with gyro: machine at 0.65–0.68× free, so speed 1 is not reachable; 0.497/0.251/0.100×. Without gyro (free 2.6×): **1.000×, 0.999×** at speed 1, 0.500/0.250/0.101× |
| F MSP on USART1 while polled (uart_evidence.py) | rc → fc.rc_us 0.09 s; host stream 52/52 good replies | 0.09 s; 68/68 good replies in the same host time |

What it means: the firmware's own CPU figure does not change (RotorFlight's `CPU:` is the task
execution time per second measured with `micros()`, not its idle time, and the tasks run exactly as
before). With the gyro loop running, what is left is the firmware's real work at 4 kHz (gyro, filter,
PID, DShot, ISRs, MMIO-heavy) plus one idle pass per skip; it no longer depends on the icount shift
(shift 3 is not faster than shift 2 with the skip on), so real time with the stock RS7 setup still
needs a ~1.5× faster host or a cheaper firmware path (e.g. the ICSR read in `micros()` is
`nvic_rettobase`, 7 % of host time). Not done: skipping the p = 1 gyro-target spin (≤ 5 % of the
time), requiring less than one full cycle between skips (the detection passes are ~6 % of the
executed instructions).

## Measurements (homelab, Intel i7-4790 @3.6 GHz, QEMU 11.1.1, stock ELF, no gyro, idle scheduler)
| mode | 10 virtual s → host s | realtime factor |
|---|---|---|
| `-icount shift=2,sleep=off` (250 MIPS) | 19.9–20.1 | 0.50× |
| `-icount shift=2,sleep=off`, MSP build + board config | 19.2 | 0.52× |
| `-icount shift=3,sleep=off` (125 MIPS) | 10.2 | 0.98× |
| no icount (virtual clock = host clock) | 10.0 | 1.00× (by construction; the scheduler keeps its 10 Hz system task) |
| Renode 1.17 reference (Ryzen 7 5800X, 216 MIPS) | 86 (3 s → 25.8 s) | 0.12× |

**With the gyro running** (2026-09-25, same host; MSP build + board config, `speed.py --icount shift=2,sleep=off`,
A/B in one session with `-M nexus-xr,imu=off`; `evidence/speed-gyro-2026-09-25.log`):

| mode | 10 virtual s → host s | realtime factor | firmware `status` |
|---|---|---|---|
| IMU on: 4000 samples/virtual s, EXTI → DMA reads, GYRO/PID tasks at 4 kHz | 21.6–22.0 (3 runs) | 0.45–0.46× | `CPU:6%`, `GYRO rate: 4000`, `cycle time: 250` |
| `imu=off` (empty SPI1, `NOGYRO`) | 19.0–19.1 (3 runs) | 0.53× | `CPU:0%`, `GYRO rate: 0` |

The live gyro loop costs ~14 % host time at 250 MIPS. Without `-icount` the guest gets only what the host
can execute per wall-clock second: there the GYRO task ran ~1/3 of the samples and `status` showed `CPU:26%`,
so the gyro-loop numbers are only meaningful under icount.

**With everything running** (2026-09-25, same host; RS7 profile, `fdm=on`, `-icount shift=2,sleep=off`,
`flight.py --plan liftoff --until 62`, host time over virtual 40..62 s — armed, governor at 1800, airborne;
`evidence/speed-flight-2026-09-25.log`):

| mode | 10 virtual s → host s | realtime factor |
|---|---|---|
| gyro 4 kHz + DShot/bidir eRPM + servos + CRSF + FDM 1 kHz, no polling (`--quiet`) | 25.8, 26.2 (logged); 25.2 in the 17:25 run | 0.38–0.40× |
| same, MSP (STATUS/ATTITUDE/DEBUG) + HMP polled every 0.25 host s | 25.3 (logged); 25.4 in the 17:25 run | 0.39× |

Next to idle (20.0) and gyro only (21.6–22.0), the flight loop costs another ~17 %.

With icount the busy-wait scheduler executes a fixed 250 M instructions per virtual second; this host
retires ~125 M of them per host second (the idle loop is MMIO-heavy: SysTick + DWT reads), i.e. ~4×
the Renode throughput on a slower CPU.

**icount shift with the idle skip on** (2026-09-26, same host, user's core running next to it; RS7 session,
`time.idle_skip: true`; `scripts/shift_evidence.sh`, `evidence/shift-2026-09-26.log`). Idle = speed.py on the
pad, disarmed, two runs; flight = `flight.py --quiet` over virtual 40..62 s (and MSP-polled `--csv` run);
firmware = CLI `tasks`/`status` on the pad at t > 15 s:

| shift | MIPS | idle, host s / 10 virt s (skipped) | flight, host s / 10 virt s (quiet; polled) | firmware `CPU:` | notes |
|---|---|---|---|---|---|
| 1 | 500 | 15.5, 15.3 (88 %) | 15.4; 15.7 | 4 % (total 4.0 %) | PID max/avg 15/8 µs; all rates nominal; boot is not idle, so 0..62 s took 136 host s (vs 112 / 103) |
| 2 | 250 | 15.8, 15.7 (77 %) | 16.2; 17.3 | 8 % (total 7.7 %) | reference: PID 28/15 µs, same as the idle-skip evidence; closest to the real 216 MHz F722 |
| 3 | 125 | 15.6, 15.7 (58 %) | 15.3; 18.2 | 17 % (total 15.8 %) | PID 56/31, SERIAL max 448 µs, `late` counts up (ACC 9, TELEMETRY 11), ACC 977 Hz; GYRO/PID 4003, FILTER 2000, RX 250 still nominal; realtime:true reaches only 0.58–0.61× |

Flight equivalence (vs `evidence/flight-2026-09-25.log`): all three shifts lift off at 33.82 s, FDM
\|roll\| ≤ 8.37–8.38°, \|pitch\| ≤ 9.16–9.17°, head speed and governor current 1793–1802 rpm, hands-off MSP
≤ 1.0 / 0.4°, altitude at 62 s 3.60 / 3.57 / 3.56 m (≤ 1.2 % apart). The FDM log md5 differs per shift, as
expected. What it means: with the skip on the host cost is the firmware's real work, which is fixed per virtual
second whatever the MIPS (the skip removes the difference as idle time), so the shift buys no speed — idle and
flight are ~15.5 host s / 10 virt s at 1, 2 and 3. A slower shift only makes the firmware look more loaded
(CPU and task times scale with 1/MIPS); shift 1 costs nothing at steady state, only a slower boot. B/D/E ran
with a third core on the host (load 2.3–2.9), so their spread is contention noise; A is the clean comparison.

### Host-time ledger (branch `prof`, 2026-09-28)
`-M prof=on` (`PROF=1 run.sh`, default off) keeps a ledger of where the host's time goes: every MMIO
region, machine timer, socket/wire path, the control server, the FDM, and — through the observe-only
QEMU hooks of `patches/prof-qemu.diff` — every vCPU slice end with its reason and due timer, the
vCPU → main loop wake latency, kicks, contended BQL acquisitions and the main loop's poll. The control
server's `prof` command (`docs/protocol.md`) returns it; `scripts/prof.py run|show|diff` records,
prints and compares runs. RS7 liftoff on the homelab: 30 823 hand-offs per virtual s (66 % reached
through idle skips, 24 % by executed instructions, 9 % timer re-arms; `crsf.frame` sets 35 % of the
deadlines), 25 720 main-loop wakes per virtual s (p50 4.2 µs, p99 39.9 µs), the vCPU blocked on the
BQL 6.3 µs after each (8.6 % of wall); FDM log byte-identical with prof on and off; overhead +2.5..+4.9 %
(pooled +3.5 %, over the 3 % target). Details, how-to and the first findings:
`evidence/prof/README.md`.

### Speed fixes (branch `speed`, 2026-09-28)
Measured with the ledger on the RS7 liftoff (`scripts/speed_evidence.sh`, `evidence/speed/README.md`):
- **No main-loop wake per virtual timer** (`patches/prof-qemu.diff`, `qemu_clock_notify_nonempty()`):
  25.8 k → 0.35 k wakes per virtual s, `vcpu.bql_wait` 162 → 5 ms per virtual s; FDM log byte-identical;
  16.23 → 12.81 host s per 10 virtual s (prof off, 3 runs each).
- **CRSF: one deadline per frame** (see "CRSF receiver" above): `crsf.frame` 9 718 → 250 deadlines per
  virtual s, hand-offs 30.8 k → 22.1 k; flight equivalent (not bit-identical: the frame's bytes arrive
  together), deterministic; 12.80 → 9.50.
- **Gyro phases**: kept on their timers (both variants without them were slower).

### Bench in the machine (branch `core-session`, 2026-09-28)
`-M nexus-xr,session=<bench>[,root=<dir>][,session-check=on]`: the machine resolves the bench itself,
in C (`devices/src/session.c`, `devices/include/fb/session.h`; JSON through the reader added to
`devices/src/json.c`), so the app can start the core on Windows with one fixed command line
(`docs/protocol.md` "Launching the core") and no shell or python. The resolver is a port of
`scripts/session.py` + `wiring.py` + `mkdefaults.sh` + `manifest.py` with the same rules, check
order and texts (Python value reprs included); those scripts stay as its reference implementation.
At machine init, before anything else: resolve → `session: ` warnings → on errors `session: <error>`
lines and exit 2 → otherwise set each resolved option unless it was given with `-M` (the class's
setters are wrapped to record what was given), take `fw=`, the blob and the manifest (in memory;
`defaults=`/`manifest=` override them), check `-icount` against `time.icount_shift`. `run.sh --session`
reads the `launch:` line of `session-check=on` and execs the command line; the bare `run.sh <elf>`
path is unchanged. `devices/build/fb_session` prints the same report on the host.

| check | how | result |
|---|---|---|
| equivalence | `make -C core/qemu/tests equiv QEMU=<qemu-prof>`: `session-check=on` vs `tests/session_ref.py` (session.py run mode + `--check`, mkdefaults.sh, manifest.py) on `sessions/*.json` (3), `tests/benches/*.json` (3: old slot form with the app's empty props and repo path; new form with the onboard receiver and baro detached, receiver on PORT-C, OSD on Int.Rx, reversed servo, environment, `profile_text`, realtime, shift 3; a WiringError bench with five errors), `app/tests` benches (none: only `manifest-fixture.json`), 122 generated (every `test_wiring.py` case, aircraft mistakes, environment/time/ports/profile variants) | 128/128: exit, `session:` lines and the whole report byte-identical - assembly lines, `machine:`, device `config:`, the -M string, icount/realtime/ports, the manifest (8585 B) and the defaults blob (5354 B rs7, 5691 B rs7-osd) byte for byte. Two tool messages of mkdefaults.sh (bash's "No such file", "defaults too big") are mapped to the resolver's words by `session_ref.py` |
| unit tests | `make -C core/qemu/tests test` (`test_session`, host, no QEMU) | 132 checks: both forms and migration (old = new = the legacy options; assumed receiver/ESC/servos notes), every WiringError of `test_wiring.py`, soldered/label cases on a modified board.json, aircraft checks incl. a JSON syntax error with line/column, OSD, environment precision, time/ports errors, `~` / `~\` / `~user`, relative root (`../../../core/./qemu/../..`), `FB_ROOT`, root found from the bench's directory, blob header/limit/boot-bare, manifest = manifest-only |
| memory | the unit tests and all 125 benches under ASan + UBSan + LeakSanitizer | clean (found and fixed a use-after-free in `fb_session_free`) |
| flight | `speed_evidence.sh SE` (run.sh `--session sessions/rs7.json` → `-M session=`, liftoff plan to 45 s, ports 31280-31284) | FDM log `c5a2b9bb6c49`, byte-identical to `main`; 9.54 host s per 10 virtual s (main: 9.74) |
| OSD | `rs7-osd.json` (ports 31280/31281) through `run.sh --session`, `subscribe {osd:true}` | the machine sets `osd=uart4`; `osd` messages (18×50 canvas, link up) from t = 1.4 s |
| override | `MACHINE_OPTS=baro=off run.sh --session` | `session: -M baro given: the bench's baro=on is not applied`; `qom-get /machine baro` = false, `osd` = uart4, `crsf` = uart5 from the bench |
| manifest | `run.sh --manifest` (session-check, no bench) vs `scripts/manifest.py` | byte-identical |
| suites | `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test`, `test_wiring.py` (45), py_compile | pass |

Not verified here: the `_WIN32` branches (UTF-16 file names, `_wgetenv`/`_wgetcwd`, `FindFirstFileW`,
`USERPROFILE`) - no Windows or mingw compiler on this host; they need a build on the Windows box.
The bench root default differs from "two levels up" literally: it is the nearest ancestor with
`boards/<board>/board.json` (for `core/qemu/sessions/` that is three levels up; for `<root>/<dir>/`
two). `~user/...` is not expanded (Python's `expanduser` would, on POSIX).

# Architecture

## Layers
```
+-----------------------------+        socket        +-----------------------------+
|  app (Godot .NET)           | <------------------> |  core (headless)            |
|  render, UI, wiring graph,  |  state: pose,        |  emulator backend           |
|  transmitter as joystick,   |  actuators, OSD,     |  peripheral models          |
|  OSD panel, launcher        |  telemetry, log      |  FDM, wiring, time control  |
|                             |  cmds: RC channels,  |  UART -> TCP/pty for the    |
|                             |  scenario, pause/step|  firmware configurator      |
+-----------------------------+                      +-----------------------------+
                                                                 |
                                                        devices/ (C library)
                                                        sensor register machines,
                                                        CRSF, MSP DisplayPort,
                                                        DShot decode, GPS, ESC telemetry
```

## Core
- **Emulator backend.** QEMU: one machine per board in `core/qemu/` (`nexus-xr`, `speedybee-f405-wing`),
  peripherals as C models in the machine that delegate behaviour to `devices/`.
- **Board** = MCU (memory map, platform file) + the firmware's unified/board config, which already
  lists every resource-to-pin assignment. The wiring UI is seeded from that config.
- **Devices** attach to board *slots* (`gyro1`, `motor1`, `servo2`, `uart4`, `i2c2`), not to raw
  pins. A slot may carry more than one bus (a gyro slot = SPI + CS + EXTI).
- **Bench → machine.** The app's bench (`docs/ui.md`: plate + firmware + aircraft + wiring; the JSON
  of `docs/protocol.md` "Session description") goes to the core as one file: `qemu-system-arm -M
  nexus-xr,session=<bench>,root=<dir>` plus QEMU's own flags (`-icount`, the sockets; `docs/protocol.md`
  "Launching the core"). The machine resolves it before init, in C and without the emulator
  (`devices/src/session.c`, host-tested): board = the unified config + `boards/<id>/board.json`,
  devices = the C descriptors, the wiring rules (both bench forms, onboard/detached, props,
  actuators) → the device options it then sets on itself, the aircraft file for the FDM, the
  custom-defaults blob and the manifest (both in memory). `session-check=on` prints the result and
  exits (the app validates a bench with it). Nothing on the launch path is shell or python, so the
  app can start, configure and stop the core itself on Windows. The Python scripts that did this
  before (`core/qemu/scripts/session.py`, `wiring.py`, `mkdefaults.sh`, `manifest.py`) are the
  reference implementation; `core/qemu/tests/test_session_equiv.sh` requires the same bytes from both.
- **FDM** integrates the aircraft each step and exposes body state (attitude, rates, accel,
  position, velocity, pressure altitude, magnetic field, ground contact). Sensor models sample it.
- **Aircraft classes.** Each aircraft (`models/<id>/aircraft.json`) names its FDM in `fdm`:
  `heli` (`fdm/src/heli.c`, the RS7) or `plane` (`fdm/src/plane.c`, the trainer: aileron L/R,
  elevator, rudder, motor). Both FDMs have the same shape so the machine can pick one by that key:
  `new(params, dt)`, `step(pwm[16])` with the FDM's four servo inputs in `fdm_inputs` order (the
  wiring maps actuator names to outputs and applies `reversed`), the motor speed handed in from the
  one ESC model (`set_rotor_rpm` / `set_motor_rpm`) and its load handed back (`rotor_load` /
  `prop_load`), `return_to_pad`, `get_state`, `euler`, and an IMU helper that includes the ground
  reaction. `fdm_params` in the aircraft file are the FDM's parameter struct by field name. What is
  class-specific stays in the class: the heli's head speed and crash rule, the plane's launch and
  hold constraints and its environment scenarios (`models/<id>/env/*.json`, `docs/protocol.md`
  `env.scenario`; a plane's default is a launch, `env_default`). The machine is wired for `heli`
  only today; the `plane` hook lands with the F405 machine (phase 3 of
  `docs/spike-inav-plane-2026-09-30.md`), the FDM side is ready and host-tested (`fdm/README.md`).
- **Time.** Lockstep: the core advances virtual time in fixed steps; sensors and the FDM update in
  virtual time; nothing depends on host speed. Real-time mode throttles to wall clock when the host
  is fast enough.
- **SoC and board descriptions.** The QEMU backend has one machine source with the peripheral
  models (`core/qemu/hw/arm/nexus_xr.c`) and describes chips and boards as data
  (`core/qemu/hw/arm/stm32_soc.c.inc`): an `FbSoc` is the chip (CPU type, clock tree with per-bus
  timer clocks, flash with its sector table, RAM regions, UID/flash-size addresses, NUM_IRQ, and
  the peripheral instances with base address, IRQ and model variant: F7 or F4 USART layout, F7 or
  F4 SPI, I2C v2 or v1), an `FbBoard` the board on it (QEMU machine type, chip selects of the IMU
  and OSD chip, outputs as timer channels, receiver USARTs, the MSP/CLI UART, blob or CLI
  configuration, config sector, idle-skip trigger). A board is one QEMU machine type:
  `nexus-xr` (STM32F722, RotorFlight) and `speedybee-f405-wing` (STM32F405, stock iNav). Adding a
  board on a known chip is data; a new chip family adds model variants where the register layout
  differs. The firmware's configuration follows the firmware: RotorFlight gets a custom-defaults
  blob in flash, iNav (no unified config, no defaults area) a CLI script typed over its MSP UART
  and saved, kept as a sector snapshot per firmware + script (`core/qemu/README.md` "Machines").

## App
- Godot .NET. Uses Godot Control nodes; the wiring graph is a `GraphEdit`.
- Launches the core with a generated bench: spawns `qemu-system-arm` with the command line of
  `docs/protocol.md` "Launching the core" (validated first with `session-check=on`); keeps working if
  the core is remote.
- Transmitter: any HID joystick (a real TX over USB included), axis→channel mapping like a
  receiver would provide.
- OSD: parses MSP DisplayPort from the emulated "VTX" UART and draws the canvas.

## Performance rule
In an emulator, one peripheral register access costs on the order of 100 instructions (measured:
Renode ~0.8–1.6 µs per access vs ~11 ns per instruction; QEMU ~0.24 µs vs ~2 ns). Models must be
frugal with registers: prefer DMA-buffer-level decoding (DShot) over bit-level PWM, batch where the
hardware would.

Deadlines cost too. Under `-icount` every virtual-time deadline ends a vCPU slice (a few µs of host
time each; `evidence/prof/README.md`). A device model should arm one deadline per event the firmware
must see at its exact time, not one per wire-level unit: the CRSF receiver delivers a frame as a burst
with one deadline per frame and releases each next byte on the firmware's own RX handshake (falling
back to the byte's wire time for a driver without that handshake) instead of one timer per 420 kbaud
byte (`core/qemu/README.md`). The QEMU patch also keeps a due virtual timer
from waking the main loop when no AioContext has a timer (`core/qemu/patches/README.md`). But a
deadline is not free to remove when it shapes where the idle skip stops: without the gyro's INT1
pulse-end and DMA-completion deadlines the skip jumps past the scheduler's gyro target and the
firmware busy-waits longer (`core/qemu/evidence/speed/README.md`, fix 3).

Polling a register is dearer than its handler. Under `-icount` a device access must be the last
instruction of its TB; found mid-TB, QEMU rewinds to it (a TB search by host PC, an unwind-data
decode, a longjmp) and runs it as a TB of its own, at every execution. A handler that tells the
chardev layer it can take input (`qemu_chr_fe_accept_input`) wakes the main loop, which then takes
the BQL from the vCPU. iNav polls `SCB->ICSR` and `SysTick->VAL` in `micros()` millions of times per
virtual second (its boot is `delay()` loops), so these paths set its speed, not the models: the QEMU
patches notify only when RXNE really clears, keep ICSR's derived bits instead of scanning all
vectors per read, and memoize the rewind (`core/qemu/patches/README.md`,
`core/qemu/evidence/speed-inav/README.md`). What they may not do is change what the guest observes,
and the idle skip observes more than it seems: its signature takes the PC QEMU last stored, i.e.
depends on how QEMU cuts TBs, so a faster TB shape (`-M io-tb=on`) is a new trajectory, not the same
flight faster.

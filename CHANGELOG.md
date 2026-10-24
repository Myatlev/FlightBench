# Changelog

## Unreleased
- **macOS (Apple silicon)**: the core builds and packages on macOS (`core/qemu/build.sh`, `core/qemu/tools/package-mac.sh`),
  the app exports as `FlightBench.app` with the core and data inside (`app/tools/export.sh`; signed with a Developer ID,
  notarized and stapled; arm64 app and core, 65 MB zip). Same bench, same two live-check PASS lines. The gate suite runs on macOS with its own
  flight baselines (`docs/contributing.md`).
- Ctrl+L clears the CLI log on every platform (macOS also takes Cmd+L).

## v0.1.0 — 2026-10-24, first release

One bench ships: **RS7 · First flight** — the official RotorFlight 4.6.0 release hex for the RadioMaster
Nexus-XR (STM32F722), booted unmodified on an emulated board and flying a Goosky RS7 Ultra (700-class
helicopter) flight-dynamics model. Windows build, self-contained: app, core and firmware in one zip.

What works (each line is backed by an evidence file, see `docs/status.md`):
- The unmodified official firmware hex boots on the emulated MCU and answers MSP on a UART through one
  custom-defaults `serial` line — no fork, no SITL port.
- The RotorFlight Configurator connects over TCP (**Copy configurator address** in the top bar).
- A virtual transmitter — keyboard or a USB joystick — with the firmware's own arm / mode switches read
  from its MSP mode tables.
- The RS7 spools up and lifts off in deterministic lockstep; two runs of the same bench give byte-identical
  flight-dynamics logs on a given platform.
- The flight models are FlightBench's own, written from first principles (`docs/fdm.md`): a rotor disc by
  blade-element theory with momentum inflow for the helicopter, the linear-coefficient model for the plane;
  parameters are geometry and data-sheet figures, not tuned coefficients. Inverted flight is the same physics
  upside down (an earlier model ran away there).
- The firmware's configuration shown as explicit layers (board / aircraft / device / bench / what the
  configurator changed), bench state kept across restarts, Signals with CSV recording, the Log / CLI
  window, fault injection (link loss, frozen baro, lost rpm).

Known limits (details and the evidence behind each in `docs/status.md`):
- Windows only.
- No OSD preset: the official `STM32F7X2` target has no OSD compiled in. The OSD window and FPV camera
  exist; a preset on a firmware build with OSD is in the repository, not in the release.
- No USB: MSP and the CLI go over an emulated UART.
- Linux and Windows cores are each deterministic but their logs differ from each other; root cause open.
- The models' estimated parameters (head stiffness, Lock number, drag areas; the trainer's aerodynamic
  derivatives) are not identified against a real flight log: by feel first, then from data.

In the repository but not in this release: iNav 9.1.0 on a fixed-wing bench (SpeedyBee F405 WING), the
OSD preset. Planned next: a Betaflight quad bench, the "add a board" guide, macOS.

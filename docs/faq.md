# FAQ

## Is this a SITL?
No. A SITL target rewrites the firmware's hardware layer to run natively on your OS
(`#ifdef SIMULATOR_BUILD`), which means every place that rewrite behaves differently from the real board
becomes a debugging session unrelated to your actual feature. FlightBench instead emulates the real
Cortex-M microcontroller and runs the unmodified firmware binary on it — same binary, same board config,
same scheduler, same timing as the real hardware. See `docs/decisions.md`.

## Why QEMU, and not SITL or HIL?
Emulating the real MCU removes a whole class of "simulator behaves differently from the board" bugs that
a rewritten SITL target introduces. Hardware-in-the-loop needs the real board; this needs none. QEMU was
chosen over Renode (the original main line) after a time-boxed spike: the QEMU machine booted the
same firmware at several times Renode's speed on the same host. See `docs/decisions.md`.

## Does the firmware know it's being emulated?
No — nothing in the firmware is aware it is running in QEMU. The v0.1 preset runs the official
RotorFlight 4.6.0 release hex, byte for byte the file you would flash onto a Nexus-XR; the only
board-side difference is one `serial` line in the board's custom defaults that routes MSP to USART1
(a real Nexus-XR talks MSP over USB, which the bench does not model). See `docs/status.md`.

## Can I use my own firmware build?
Yes — that is the point. Point a bench at your own ELF (preferred, for symbols) or HEX in the wizard, or
use the Firmware window's "watch file" to reload it automatically after every rebuild. See
`docs/firmware-dev.md`.

## Why is the configurator slow to connect right after a reboot?
A configurator save reboots the firmware, and the emulated boot is not instant — up to about 30 seconds on the
RS7 bench, depending on the host. The top bar shows a "firmware booting — connect in ~N s" hint next to the configurator
address while this is happening; wait for it (or the green dot in the configurator itself) before
retrying a connection.

## Is it deterministic?
Yes, within one platform: the same bench flown with the same pilot script produces a byte-identical
flight-dynamics log, run after run, because the core advances virtual time in fixed instruction-counted
steps (QEMU's `icount`) rather than following the wall clock. This is also how regressions are caught —
`core/qemu/scripts/gates.py` re-flies a fixed set of benches and compares log hashes against known-good baselines.
Note that Linux and Windows builds each produce their own consistent hash, and the two do not currently
match each other — see `docs/status.md`.

## Does it run on Linux? macOS?
The core (the emulator, headless) builds and runs on Linux from source, and is verified deterministic
there. It also builds and runs on Windows (MSYS2). The graphical client — the Godot app with the flight
screen, wizard and windows — is Windows-only today; on Linux you run the core alone, connected to a
script or a Windows client over the network. macOS support is planned.

## What boards and firmwares does v0.1 support?
One bench: the RadioMaster Nexus-XR (STM32F722) running the official RotorFlight 4.6.0 release hex,
flying a Goosky RS7 Ultra helicopter model. See the Supported table in `README.md` and `docs/status.md`
for exactly what is verified.

## Is there an OSD?
Not in v0.1. The official RotorFlight target for this board has no OSD compiled in, so an OSD preset
would need a custom firmware build; that is planned for a later release, with the OSD window and the
FPV camera that already exist in the app.

## Other firmwares? Fixed-wing? Quads?
Planned: iNav on a fixed-wing bench next, then Betaflight on a quad, then ArduPilot. See `README.md` →
Status/roadmap.

## Is the flight model realistic?
The RS7's helicopter FDM is a conventional-frame model (thrust from collective and head speed, cyclic →
rotor-cone dynamics, tail rotor, stability derivatives) ported from a real RC-helicopter SITL bench.
Where a parameter is a placeholder, the aircraft's README says so (`models/rs7/README.md`). Treat it as
good enough to validate firmware logic and tuning direction, not as a substitute for a real test flight.

## What's the licence?
GPL-2.0-or-later (`LICENSE`). The core's machine is compiled into QEMU, which is GPL-2.0 as a whole, so
FlightBench's own code is licensed to be combined with it; Godot is MIT. The bundled firmware binary is
RotorFlight's own GPL-3.0 software, executed by the emulated MCU — never linked into FlightBench;
`firmware/FIRMWARE.md` gives its exact source and checksum.

## How fast does it run compared to real time?
It depends on the host. The RS7 bench on a modern desktop CPU (Ryzen 5800X) runs at roughly 1.6–1.9×
real time; older or busier hosts are slower. A **Real-time** setting throttles a fast host back to 1.0×
for flying by hand; turning it off lets a regression or CI run go as fast as the host allows. See
`docs/architecture.md` → Performance rule.

## What if my firmware won't arm, or behaves oddly?
FlightBench does not paper over that — if a firmware refuses to arm with a wiring or config problem, the
bench shows exactly that, the same as a real board would. Check the Status window's firmware line (it
names arming-blocker flags), the Config window's live diff against what you expected, and
`state.no_signal` in the Signals window for any output the firmware itself is not driving. See
`docs/decisions.md` → "The bench does not paper over firmware behaviour".

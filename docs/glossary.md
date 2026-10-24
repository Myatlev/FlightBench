# Glossary

Terms as FlightBench uses them. See `docs/ui.md` and `docs/protocol.md` for the full definitions this
page summarises.

**Aircraft** — one modelled airframe: an FDM type, named actuators, sensor mounting, environment
scenarios (`models/<id>/`). One aircraft can be flown on different boards/firmwares by changing only the
wiring. See `docs/architecture.md` → Core.

**Bench** (стенд) — board + firmware file + aircraft + wiring + board config (diff) + window layout, saved
as one JSON file; the unit you open, edit and fly. See `docs/ui.md`.

**Bench state** — the firmware's config sector and metadata, saved per bench so calibrations and
configurator changes survive closing and reopening the bench, restored (as a delta, not a full replay) on
the next start. See `docs/firmware-dev.md` → "Bench state".

**Board** — an MCU plus its pin/peripheral map: `boards/<id>/board.json` names every pad, onboard device
and the QEMU machine type it runs on. See `docs/architecture.md` → Core.

**CLI** — the firmware's command-line interface, reached over its MSP/CLI UART; FlightBench's Log/CLI
window sends lines to it directly, and CLI-configured boards use it to apply their config script. While
the firmware is in its CLI, MSP (and so the configurator) cannot connect. See `docs/firmware-dev.md`.

**Client** — anything talking the core↔app protocol over the control-server socket: the Godot app, or a
script (`docs/protocol.md`).

**Collective** — the helicopter rotor's blade pitch angle (degrees), set by the swash servos through the
aircraft's mixer; reported in the state stream as `heli.coll_deg` and shown in the Aircraft window's rotor
row alongside rotor thrust. See `docs/protocol.md` → `state`.

**Core** — the headless emulator process: MCU emulation, peripheral models, the flight-dynamics model,
and the control server the app talks to. Runs standalone (CI, a server) or launched by the app.
See `docs/architecture.md`.

**CRSF** — Crossfire, the serial protocol a modelled receiver uses to deliver transmitter channels to the
firmware; also carries telemetry back.

**Device** — an emulator-independent model of one piece of hardware (a sensor, an ESC, a receiver, an
OSD chip), implemented once in `devices/` and reused by whichever emulator backend is wired to it.

**Diff** — the firmware's own `diff all` output: every setting that differs from its compiled-in default,
as the firmware itself reports it. Shown live in the Config window, kept in the bench as a record of
what was configured.

**DisplayPort OSD** — the MSP DisplayPort protocol a firmware uses to draw an on-screen-display canvas
character by character on a digital video system; FlightBench models the video side as a device and
renders the canvas in the app. No v0.1 preset drives it (the official firmware has no OSD compiled in).

**DShot** — the digital protocol FlightBench's ESC model decodes for motor commands, including
bidirectional DShot's return eRPM telemetry.

**FDM** — flight-dynamics model: the physics integrator that turns actuator positions (servo/motor pulse
widths) into the aircraft's motion and feeds it back to the emulated sensors. One FDM class per aircraft
kind (`heli`, `plane`); see `fdm/README.md`.

**First start** — the very first time a bench's firmware+script combination boots with a blank
config: a CLI board types its assembled script over the CLI and saves; some aircraft also run their
`auto_first_start` functions (a calibration) right after. A blob board (its config is baked directly into
the flash image) has no scripted first-start sequence at all. See `docs/firmware-dev.md`.

**Governor** — RotorFlight's closed-loop head-speed controller, which holds the RS7's rotor at a target
rpm regardless of collective load; its state is exposed in the firmware's telemetry.

**icount** — QEMU's deterministic instruction-counting mode, the mechanism lockstep time relies on: the
core advances virtual time in fixed steps tied to executed instructions rather than the wall clock, so
the same bench and pilot script give the same result every run. See `docs/architecture.md` → Performance
rule.

**Layer** — one named source of firmware config lines (board, aircraft, profile, device, bench), applied
in a fixed order with later layers winning a conflict. See `docs/firmware-dev.md` → "The config layers".

**MSP** — MultiWii Serial Protocol, the binary protocol both supported firmwares answer status/config
requests on; the mechanism the stock configurator, and the core's own status polling, both use.

**Model / profile** — "model" is used loosely for an aircraft (see Aircraft); a "profile" is a firmware
profile — a CLI text file or pasted `diff all` text applied as one of the config layers, e.g. a PID or
rate profile.

**Pad** — one named connection point on a board (a pin, or a bundled group of them for a bus device), the
unit wiring connects a device to; `boards/<id>/board.json` lists every pad with its kind and flags.

**Preset** — a ready-made bench shipped with FlightBench, so a fresh install can fly at once; opening one
copies it into your own benches folder, leaving the bundled file untouched. See `presets/README.md`.

**Realtime factor** — virtual time divided by host (wall-clock) time over roughly the last second of
simulation; 1.0× means the bench is keeping pace with a real clock. Independent of `icount`'s determinism
— a bench can run in lockstep far faster or slower than 1.0× when the **Real-time** throttle is off.

**Session-check** — validating a bench without starting it: the core resolves the whole bench (wiring,
config layers, firmware path) and prints a report, or rejects it with `session:` error lines and exits 2.
This is what the app runs before every start. See `docs/firmware-dev.md`.

**Snapshot** — taking the firmware's current `diff all` (whatever the configurator changed included) and
writing it as the bench's own lines (layer 5), so it becomes the bench's new starting point
(Config → **Snapshot firmware config → bench lines**). See `docs/firmware-dev.md` → "The config layers".

**Wiring** — which device is connected to which pad, plus per-device properties (reversed, telemetry
source, sensor mounting); one of the three files/blocks that make up a bench, editable as a graph in the
wizard. See `docs/ui.md` → "New bench: five steps".

# presets/ — bundled benches

A **preset** is a ready bench shipped with FlightBench, so a fresh install can pick one and fly at once.
Each `*.json` here is a bench in exactly the form the wizard saves to `user://benches/<name>.json`
(`app/scripts/Bench.cs`: board, firmware, aircraft, wiring as device.port → pad entries, board config /
profile, core time, window layout, camera), plus an optional one-line `description` for the list.
Optional bench fields a preset may use (the wizard does not write them yet, except `transmitter`):
- `visual`: the aircraft's visual scene, relative to the data root (default `models/<aircraft>/visual/<aircraft>.tscn`);
- `switches`: the model's old switch layout, `[{"channel": 5, "name": "ARM", "note": "…"}, …]` (1-based CRSF
  channels; informational since 2026-10-23: a bench without `transmitter` keeps these names as labels of `auto`
  channels; the Transmitter window's rows come from `transmitter` + the firmware's modes);
- `switchOrder`: one line, the order to use them in (shown under the switches);
- `transmitter`: the radio's switch per channel 5..16 (`app/README.md` → Benches → Transmitter model), keyed by the
  1-based channel: `{"5": {"type": "auto"}, "6": {"type": "switch2", "label": "throttle"}}`; `type` = `auto` (a switch
  from the firmware's modes, a row once the firmware gives ranges), `switch2` / `switch3` / `switch6` (a static switch:
  1000 / 2000, 1000 / 1500 / 2000, ArduPilot's six flight-mode values), `label`, `start` (static: start position,
  0 = lowest). Absent channel / absent block = `auto`. The RotorFlight presets (`rs7-first-flight.json`,
  `rs7-fpv-osd.json`) carry `5` auto + `6` switch2 `throttle` (RotorFlight's channel 6 is the throttle the governor runs
  off: key 6 = throttle on); the iNav presets carry none (all `auto`: their switches come from `inav.cli`'s modes);
- wiring `props` may be numbers / booleans (`"accel_offset_mg": 20`) as well as strings.
- `benchCli`: layer 4 of the firmware's config script, the bench lines (`./<id>.cli`, next to the preset file);
- `disabledLines`: lines of the assembled script the core skips (exact texts or `layer:index`);
- `aircraftFile`: the bench's own copy of its aircraft (`./<id>.files/models/<aircraft>/aircraft.json`), when an
  exported bench had modified the aircraft's files;
- `preset`: set by the app on the copy it makes when a preset is opened (not in a preset file).

| file | name | what |
|---|---|---|
| `rs7-first-flight.json` | RS7 · First flight | Nexus-XR, the bundled RotorFlight 4.6.0 (`firmware/`), RS7 wiring (gyro, baro, ELRS receiver on uart5, ESC on motor1 with bidirectional DShot, servos 1–4 → swash_a/b/c, tail), local core, real-time; `transmitter` ch5 auto (ARM from the firmware's modes, key 5) + ch6 switch2 `throttle` (key 6); Status + Transmitter + Log windows |
| `rs7-fpv-osd.json` | RS7 · FPV with OSD | the same + `msp_osd` on PORT-A (uart4); OSD window, FPV camera, OSD overlay on |
| `inav-trainer-launch.json` | Trainer · NAV LAUNCH | SpeedyBee F405 WING with the stock iNav 9.1.0 release hex (`firmware/speedybee-f405-wing/`), the trainer plane: CRSF on uart1, ESC on S1 (STANDARD PWM, the configurator's airplane default), servos S3..S6 → aileron_l, aileron_r, elevator, rudder, the IMU with a 20 mg / 0.5 % accel error (the calibration removes it), the GPS module (u-blox `ublox` on uart3 + its QMC5883L `qmc5883l` on I2C1) and a 4S 4200 mAh `battery` on BAT; local core, real-time. The environment is the aircraft's `env_default`, `launch` (the app form has no env field): held 10° nose up, thrown by the `launch` command (bottom bar **Launch (L)**). `visual` = `models/trainer/visual/trainer.tscn`; `switches` ch5 ARM, ch6 ANGLE, ch7 NAV LAUNCH (labels of `auto` channels; the rows come from `inav.cli`'s modes, keys 5 / 6 / 7) + `switchOrder` (shown in the Transmitter window); Status + Transmitter + Log / CLI + Model open, chase camera. First start: the core configures the board and runs iNav's ACC and compass calibrations (~68 virtual s, a few host minutes, once per firmware + script: `cfg-cache`; Status shows the progress). Order: ANGLE + NAV LAUNCH on → ARM with the throttle low → throttle up → Launch |

| `inav-trainer-fpv-osd.json` | Trainer · FPV with OSD | the Trainer · NAV LAUNCH bench (the GPS module — `ublox` on uart3 + its `qmc5883l` on I2C1 —, the `battery`, the onboard SPL06 baro, all kept) plus a digital HD VTX OSD: `msp_osd` (MSP DisplayPort) on **UART4** (`serial 3`: the F405 WING's free UART next to the GPS — uart1 is the receiver, uart3 the GPS, uart6 the core's MSP host socket; uart2 / uart5 are free too), as `rs7-fpv-osd` does on the Nexus-XR; OSD window open, FPV camera, OSD overlay on. iNav makes an MSP DisplayPort serial port the OSD before the onboard MAX7456 (docs/spike-inav-plane-2026-09-30.md A4). **Core side (2026-09-29):** the packaged core (main) leaves `msp_osd` out on `speedybee-f405-wing` (`not modelled on this machine`) and the `msp_osd` descriptor's lines are RotorFlight's (`serial {serial} 1 115200 …`, `displayport_msp_serial`): iNav's `serial 3 33554432 …` (FUNCTION_MSP_OSD, bit 25) is not typed yet — the preset flies, its OSD stays dark until the core models it; the `presets` self-test notes it and requires the line once the core models the device there |

The wiring matches `core/qemu/sessions/rs7.json` / `rs7-osd.json` / `trainer.json` (+ `msp_osd` on uart4 for the FPV trainer).

## What a preset packs: the bench and its layer files
A bench's firmware config is layered (`docs/protocol.md`, session description): the board's base script,
the aircraft's `firmware_cli`, the profiles, the wired devices' lines, and the bench's own lines — an
optional `bench_cli` file next to the bench (plus `disabled_lines` in the bench for generated lines it leaves
out). A preset made from a bench packs **the bench JSON and its `bench_cli` file** (next to the preset, the
bench's `bench_cli` then relative to the data root: `presets/<id>.cli`); the board, aircraft and profile
files are already in the data root (their paths stay relative). The core provides what the app needs:
`{"type":"config","export":true}` answers the assembly (`assembly.layers`: every file read, by layer, with
its data-root path), the script, whether the firmware has it, and the last `diff all`; the core writes no
file. To freeze what the firmware has now (the configurator's changes included) into the preset:
`{"type":"config","snapshot":"bench","path":"<the bench's bench_cli>"}` first (the firmware's `diff all`
as the bench's lines; the firmware reboots). The shipped presets have no `bench_cli` today.

## Deleting a bench made from a preset
Main menu → **Delete…** (or the flight screen's Bench → **Delete bench…**) deletes the bench copy, its session file and
its saved firmware config (bench state); the file here is never touched. Opening the preset again makes a fresh copy.

## What a preset packs (Export as preset…)
The flight screen's **Config** window → **Export as preset…** writes a folder the user picks (app/README.md → Config layers):
- `<id>.json` — the bench as a preset (`lastUsed`, `wiringLayout`, `preset`, `modes`, `firmware_diff` dropped; a
  `description` added when it had none); its own files named with `./` (relative to this file's folder);
- `<id>.cli` — layer 4, the bench lines (`benchCli`), when the bench has any;
- `<id>.files/` — the bench's own copies of modified files (a preset's aircraft folder copied on its first edit, a board
  script), only when it has them; an aircraft copy's `firmware_cli` points at its own CLI file;
- `<id>.README.md` — the files, the config layers with their lines (board / aircraft / device / bench, disabled lines,
  conflicts: the later layer wins) as the core assembled them (its `config export` when connected), and the firmware's
  `diff all` at the export (informational).
Install: copy the folder's files into `<data root>/presets/`. **Opening** it copies the bench and the `./` files next to
the new bench (`user://benches/<id>.cli`, `<id>.files/`) and points the aircraft copy's `firmware_cli` at its new place;
the preset files are never written. The bundled presets here carry no `./` files.

## A preset's files are read-only bundles
A bench opened from a preset is marked with `preset` (the preset's id). Its aircraft / board files are the bundle's: when
the layered view (wizard summary, Config window) edits one of them, the app first copies it next to the bench —
the aircraft folder (without `visual/`) to `<benches folder>/<bench>.files/models/<aircraft>/` (the bench's
`aircraftFile`), a board script to `<bench>.files/boards/<board>/` (the bench's `boardConfigFile`) — and edits the copy.
The editor says so before saving. A bench of the user's own edits the file in the data root.

## Paths are relative to the data root
`firmwarePath`, `boardConfigFile`, `profileFile` and `visual` are relative to the **data root** (a path starting `./`
is a bench's own file: relative to the bench's / preset's folder) — the repo root in a
checkout, `<exe dir>/data` in an exported build (`app/tools/export.ps1` copies `presets/` and `firmware/`
there). The app writes them into the session file unchanged and passes `root=<data root>` to the core,
which resolves them (`docs/protocol.md` → Launching the core). Never put an absolute path in a preset.

## Opening a preset
Main menu → **Presets** → double-click (or **Open preset**): the preset is copied into the benches folder
under its `name` and the flight screen starts it (local core). The file here is never written. When a bench
of that name already exists it is opened as it is (it may carry the user's changes) and the log says so;
delete or rename that bench to get the preset fresh. `FB_PRESET=<file id or name>` opens one from a script.

## Adding a preset
1. Build the bench in the wizard (paths inside the data root become relative when picked with Browse…), fly it.
2. Copy `user://benches/<name>.json` here as `<id>.json`; add `"description"`; drop `lastUsed`, `wiringLayout`
   (empty = auto-layout) and anything machine-specific.
3. Check every path is relative and present under the data root; anything new it needs (a firmware in
   `firmware/`, a profile in `models/`) must also be copied by `app/tools/export.ps1`. A preset's own files
   (`bench_cli`, an aircraft copy) are `./<id>.cli`, `./<id>.files/…` next to the preset: `./` paths must stay in
   the folder (`./../x` is refused and not copied). `export.ps1 -Presets '<glob>'` ships a subset of the presets
   with just the firmware they name (the release profile; v0.1: `rs7-first-flight` — the stock hex alone, official firmware only).
4. `FB_SELFTEST=presets FB_CORE_EXE=<qemu-system-arm> Godot..._console.exe --path app --headless`: parses
   every preset, checks its paths (the visual too), runs the core's `session-check` on the session it produces
   (skipped when the core lacks the board's machine; the launch line's `machine` must be the board), and checks
   that opening it copies it without touching the bundle. The export (`app/tools/export.ps1`) also ships the
   boards' / models' `*.cli` and `models/*/env/*.json`, which the core reads for a CLI board and a plane.

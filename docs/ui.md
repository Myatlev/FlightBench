# UI concept (agreed 2026-09-25)

Vocabulary: a **bench** (стенд) = board + firmware file + aircraft + wiring + board config (diff)
+ window layout. The **aircraft** (model) is one part of a bench. Benches are saved presets that
can be edited and re-run in one click.

## Main screen
Buttons, bottom to top: Exit · Settings · New bench · Delete… · Edit bench · Load bench. Above them the list of recent
benches (double-click = load and start) and the **Presets** list.

**Delete…** (the selected bench; product owner's test 2026-10-01: a bench made from a preset could not be deleted):
a confirmation that names the bench file and what goes with it — its session file and its saved firmware config
(the bench-state files) — then all of them are deleted. Bundled presets are never touched: opening the preset again
makes a fresh copy. The flight screen's **Bench → Delete bench…** does the same for the open bench after stopping its
core, and returns to the main menu.

**Presets** = ready benches bundled with the app (`<data root>/presets/*.json`: the repo's `presets/`, the
exported build's `data/presets/`), one row each: name + a one-line description (the bench's optional
`description`). Double-click or **Open preset**: the preset is copied into the benches folder under its name
(`user://benches/<name>.json`; the bundled file is never written) and the flight screen opens and starts the
local core, as for any local bench. A bench of that name that already exists is opened as it is (it may hold
the user's changes) and the flight screen's log says so. Presets name their firmware, board config and
profile by paths relative to the data root, so they fly on a fresh install with the bundled core and
firmware. **First run** (no benches yet): the Presets list comes first, with the hint line "Start with a
preset"; afterwards the recent benches are on top and the presets below them.

## New bench: five steps
1. **Board** — list from `boards/` (picture, what is emulated). The user brings the firmware file
   (ELF preferred; hex accepted, ELF enables symbols). Checked against the board.
2. **Aircraft** — list from `models/` (picture, FDM type). Cubes are fine until real art exists;
   the placeholder shows body axes and labels so the orientation vs the board is visible.
3. **Wiring** — graph (`GraphEdit`): device nodes on the left ("+ device" from a catalogue), the
   board with its slots (from its unified config) on the right, wires by mouse, ports typed by bus
   (SPI / I2C / UART / motor / servo); the selected device's properties in an inspector. Seeded
   automatically from the board config,
   usually nothing to change. Device properties live here: servo "linkage reversed", ESC RPM
   source (bidir / serial telemetry), receiver protocol, sensor mounting offsets (numeric table;
   markers in 3D; drag-in-3D only once real models exist).
4. **Board config** — a text box for the real board's `diff all`, pasted as is. The sim shows the
   board as it is; mechanical reversals are wiring properties, not edits to the diff.
5. **Summary & start** — what was assembled, where the preset is stored, **Firmware config at first start**, the
   **Core** row, core time, Start.
   **Firmware config at first start — by layer** (product owner 2026-10-02: a developer tool, nothing implicit or
   read-only): the script the core puts into the firmware the first time the bench starts, as the core assembles it (a
   session-check of the bench, run when the step opens, on **Check again** and after every edit), as **layers with
   provenance** — **1 board** (the board's `defaults.cli` / `*.config`) · **2 aircraft** (the aircraft's CLI, e.g.
   `inav.cli`) · **3 profile** (the `profile` files / `profile_text`) · **4 device** (the wired devices' lines, `device
   ublox on uart3`) · **5 bench** (the bench's own lines) — in
   that order; the same setting in two layers is a **conflict** and the **later layer wins** (a conflicts group at the top,
   amber: which line wins, which are overridden). Per line: the text, its source (`models/trainer/inav.cli:25`), an **on**
   box. A line's actions: **Edit <file> at line N** (board / aircraft; the file in an inline editor — Save writes it in
   the data root, or, for a bench made from a preset, copies the preset's aircraft / board file next to the bench first
   and points the bench at the copy; the editor says which before saving), **Disable / Enable** (the bench's disabled
   lines: the core skips them), **Override in bench lines** (copies the line into layer 5 to change it there). **Bench
   lines** (layer 5): an editable box = a file next to the bench (`<bench>.cli`), **Save bench lines**. A core that does
   not report layers: the app attributes the lines to the bench's files (marked *inferred*) and says what that core
   ignores. **Copy** copies the assembled script. The summary lists `Board config (step 4): none | N lines` and the
   bench lines.
   Core time: virtual CPU speed, idle skip, **Real-time** (on by default for a new bench; a bench's saved
   value is kept).
   **Core**: radio **Local (bundled)** (default: the app starts the core on this machine on the bench's ports;
   the line under it shows what that resolves to — the path and where it came from — or "no core found — set it
   in Settings → Paths") / **Remote host:port** (connect only; the host and port fields are editable only
   here). Saved as `core: "local" | "remote"` + `coreHost` / `corePort` in the bench JSON; older benches without
   `core` are migrated on load (loopback / empty host → local, another host → remote).

## Settings (global, not per bench)
- Paths: repo root, firmware directory, **core** (`qemu-system-arm`; empty = auto, the line under it shows
  what auto found: the build's `data/core/`, else this path, else `FB_CORE_EXE`), **data root** (read-only: the
  core's `root=`), **keep the core running after closing the bench** (off by default).
- **Transmitter**: first line, the joypad as a fact — `Joypad: XInput Controller — connected` or `Joypad: — not plugged
  in (channels from the keys and the Transmitter window)`; then two stick gimbals drawn live from the joystick, channel
  bars (with the owner marks, below), the joypad picker, calibration, **Detect**, axis→channel mapping (sources `none` /
  `ui` / `axis n` / `button n` / `buttons n,m` — the joypad or the Transmitter window only; there is no `keyboard`
  source, an old one reads as `none`), reversal, min/centre/max, transmitter mode (default **Mode 2**; changes only
  which axis is drawn on which stick).
- **Keys** — the complete catalogue, in three groups, every entry bindable (Bind / Clear; a bind already used moves):
  - **Sticks** — the keyboard layer over channels 1..4: roll − / + (← / →), pitch − / + (↓ / ↑), collective − / + (S / W;
    the label says it is an integrator: the value holds), yaw − / + (A / D);
  - **Transmitter switches** — **switch 1..5** (defaults 5..9: the next position of the N-th row of the Transmitter
    window, channel order); opened over a bench each row says what it is there (`Switch 1 (ch5 ARM)`, `Switch 2 (ch6
    throttle)`), plain `Switch N` from the main menu;
  - **Application** — restart bench, pause, step, battery, reset aircraft, screenshot, cameras, body axes, launch,
    window toggles, faults.
- **The keyboard is a layer, not a device** (decisions 2026-10-23): always on beside the joypad (except while a text
  field has the keys or Settings / Edit bench is open over the bench). Two hands on one channel: the last input that
  changed it holds it — the joypad when its axis moves beyond the deadzone, a key when pressed (and released, if the pad did not move meanwhile); W continues
  from where the stick left the collective. Nothing is substituted: with no joypad the keys are simply the only thing
  moving channels 1..4.
- Settings is also reachable **from the bench** (flight screen top bar → Settings): the same page in a window
  over the flight screen; **< Back to the bench** (or **Escape** outside a text field / key capture) closes it, the
  core stays connected and the bench keeps running. While it is open the keyboard does not fly the aircraft (arrows /
  WASD / a key being bound go to Settings only). Key binds and channel sources apply when it closes.
- Ports, theme.

## Flight screen
3D view full-screen; **windows** on top (Godot embedded subwindows: movable, resizable, closable)
opened from a menu bar: Status, Aircraft, Transmitter, Log/CLI, Firmware, Signals & plots, Wiring (read-only),
Model, Faults, OSD, Functions, Config. Open windows, positions and sizes are saved into the bench.
Bottom bar: time (virtual), run/pause/step, real-time toggle, realtime factor.

### Flight screen windows (product decision 2026-10-01: one fact in one place)
Every fact has one window; nothing is shown twice.
- **Status** — compact, top-left, four lines and nothing to press:
  1. the **link**: `connected · realtime 0.39× · 97 states/s` (not connected: where; the app's core only while it
     matters: `· core exited (code 1)`);
  2. the **firmware**: `ARMED` / `disarmed` · the active flight modes as text tags `[ANGLE] [NAV LAUNCH]` (never
     toggles) · `flags: CAL NOPREARM` (arming flags) · `cpu 12%`; while the firmware does not answer MSP it says why
     (`waiting for it to answer (booting…)`, `busy with its first start`, `rebooting after the config save…`),
     `POWER OFF (Battery)`, **`in the CLI — MSP blocked`** (from `state.fc.cli` / the core's `cli` event; not guessed)
     with the window's one control next to it, **exit CLI (reboots)**, shown only then;
  3. the **sensors**: the OSD (`OSD: MAX7456 30×16 (no video in FlightBench)`, `OSD: MSP DisplayPort 50×18`,
     `OSD: none`), the firmware's baro altitude (`baro 3.21 m`, when it reports one), GPS (when the state carries it);
  4. the **config progress**, only while it runs: `config: applying the CLI script…` → `config: calibrating the
     accelerometer… pose k/6 (label)` → `config: applied · ACC calibrated`, hidden a few seconds later; a replayed
     `config: restored (saved snapshot)` (a client connecting after the start) the same way; `config: FAILED …` stays.
     With the bench state (below): `config: restored (bench state)` and, after an edit, `config: applying 4 new lines
     (delta)…` instead of a whole first start; lines the bench no longer has are logged, not reverted.
- **Transmitter** — owns all input: the radio's switches (below: the bench's static switches and the ones derived
  from the firmware's modes, one row per channel in channel order, positions, keys), the sticks, the 16 channels — each
  bar marks who last drove it: `joy` (the joypad), `kb` (a key: a stick key, or a row's switch key), `ui` (a click on a
  row), nothing until something did —, and the hint line generated from the bind table (Settings → Keys), never a
  literal: `keys: Left/Right roll · Down/Up pitch · S/W collective (holds) · A/D yaw · switches 5 6 7 8 9`, plus
  `· launch L` on a plane bench.
- **Aircraft** — the model from the FDM / core state, for both aircraft classes (a row the state lacks is hidden):
  altitude (`on the ground`), ground speed and climb, air data (plane: airspeed, α / β), motor (plane: thrust, prop
  rpm, throttle) or rotor (heli: head rpm, motor rpm, throttle), control surfaces in degrees (plane) or servos in % of
  travel (heli), the launch phase / hold / scenario and its step, the mix warning, and the red **crash** banner with
  **Reset aircraft** (a crash opens the window).
- **The core** is not a window: the **Bench menu's Core group** (below).
- **Default layout** (presets, a bench without a saved layout): Status (compact, top-left), Aircraft below it, Log /
  CLI top centre, Transmitter right (the trainer's Model window above it). Status never overlaps the Transmitter at
  1600×900 nor at UI scale 1.25. A bench saved before the Aircraft window gets it opened where the Status window's
  plane block was (below the compacted Status, or the first free spot next to it) and keeps it.

**Top bar** (product decision 2026-10-01): functional buttons on the **left** — `< Menu`, **Bench** (Edit bench…,
Save as…, Save, the **Core** group — a read-only line `Core: running · pid N · control C, msp M`, Restart core, Bench
check, Core address…, Use local core when offered, Close bench —, Functions…, Config…, Reset firmware config
to first start…, Delete bench…), **Windows**, **View**,
**Settings** (the settings page over the bench, see Settings), **Copy configurator address** (after a reboot, while the
firmware does not answer MSP, next to it: *firmware booting — the configurator can connect in ~N s*, N = the last boot
the app measured; tooltip: after "Save and Reboot" in the configurator wait for the green dot); in the **centre** a
title that is not clickable: `<bench> · <board> / <aircraft>`; on the **right** **Disconnect** (Connect / Cancel
while not connected). The keyboard shortcuts are unchanged (Keybinds; the menus show them as accelerators).

**The firmware's config is the firmware's** (product decisions 2026-10-01): the bench's shipped diff is a starting
point; the user then configures the firmware with its configurator and the app only adapts. Modes, the applied diff
and the OSD are read from the core (docs/protocol.md `modes`, `diff`, `osd`); the app never writes the running
firmware's settings behind the user's back. Since 2026-10-02 (a developer tool: nothing implicit or read-only) the
*script* the core types at a first start is edited at its source — its layers' files, the bench lines, disabled lines —
and sent only by an explicit operation (Apply script now, Reset to first start, a restart), see the **Config** window.

**The Transmitter window is the radio** (decisions 2026-10-23), not the firmware's mode table: the pilot's radio has
switches the firmware does not know about, and the firmware only reads channels. Each channel 5..16 has a type in the
bench (`transmitter`; Edit bench → Summary → **Transmitter**; absent = `auto`):
- `auto` — a switch derived from the firmware's modes (below); **no row until the firmware gives ranges on the channel**;
- `switch2` — 1000 / 2000; `switch3` — 1000 / 1500 / 2000; `switch6` — ArduPilot's flight-mode channel: 1165 / 1295 /
  1425 / 1555 / 1685 / 1815, read back with ArduPilot's thresholds ≤ 1230 / ≤ 1360 / ≤ 1490 / ≤ 1620 / ≤ 1749 / above
  (`libraries/RC_Channel/RC_Channel.cpp`, `RC_Channel::read_6pos_switch`: `< 1231`, `< 1361`, `< 1491`, `< 1621`,
  `< 1750`; checked against ArduPilot `master` 2026-10-23);
- a static switch has a `label` (`throttle`) and a `start` position (0 = lowest). The firmware's modes **label** its
  positions where their ranges cover the position's µs (the boxes active there, the rule below), never rebuild it;
  a position they do not name carries the type's own name: `low | high`, `low | mid | high`, `1..6` (ArduPilot's
  FLTMODE1..6). `—` is left to derived switches, where it means "no box here".
- a bench saved without the block (before 2026-10-23) is shown and driven with its board's family default (the file is
  rewritten only when the model is edited); its old `switches` names become labels where that default has no static
  switch and the firmware has no range. An explicit empty block is "all auto on purpose" and gets no default.
A new bench on a RotorFlight board (`firmware_family`) gets ch5 `auto` + ch6 `switch2` `throttle` (RotorFlight's map
`AECR1T23`: channel 6 is the throttle axis the governor runs off; high = throttle fully on, the head spools; low =
throttle hold); other families all `auto`. Rows: one per channel that has a switch (static ones always, `auto` ones once
the firmware gave ranges), **in channel order**; keys **switch 1..5** step them in that order (RS7: 5 = ARM, 6 =
throttle — what the row's key label shows). Header: `modes: from firmware (t = …) · ch6 throttle: bench switch`. A
static position's tooltip shows its µs and the firmware boxes active there. A position is sent only on a click / key;
a static switch starts at its `start`, never on an ARM position nobody asked for.

**Switches from the firmware's modes** (Transmitter window, `auto` channels). The core reads the firmware's mode boxes and aux
ranges over MSP (`modes`: after the firmware is ready, after its config is applied, after a reboot, and on
**Update modes**). The app derives one switch per aux channel that has ranges:
- the channel's span 900..2100 µs is cut at every range's start and end; each piece gets the boxes whose range covers
  it (a range is active for start ≤ µs < end); neighbouring pieces with the same boxes merge. The pieces left are the
  switch's **positions**, low to high — 2 (one range at an end: `— | ARM`), 3 (a range in the middle, or two stacked:
  `— | ANGLE | HORIZON`), more when the ranges say so; overlapping ranges give combined positions
  (`— | ANGLE | ANGLE+NAV LAUNCH`). Each position is labelled with its boxes joined by `+`, or `—` for none. Ranges
  that **touch** (1300–1700 and 1700–2100) are three positions for two boxes (`— | ANGLE | HORIZON`; 1700 itself is the
  upper one) and one position for the same box (`— | ANGLE`); a partial overlap (1200–1800 + 1600–2100) gives four
  (`— | ANGLE | ANGLE+HORIZON | HORIZON`). Every new `modes` message rebuilds the switches in place — no stale row, a
  channel that lost its switch is released (checked 2026-09-29 against the fake core: the app side is right; whether
  the core re-reads the modes after a configurator save is the core's part, branch `bench-state`);
- position *i* of *n* is sent as a real switch gives it (1000 + 1000·i/(n−1): 1000 / 2000, 1000 / 1500 / 2000) when
  that lies inside it, else the nearest of 1000 / 1500 / 2000 inside it, else its middle — "inside" keeping 5 µs from
  both ends, so the value never sits on a range's edge (a middle range starting at 1500 is sent at its middle, 1600
  for 1500..1700, not at 1500). The sent value always lies inside its position;
- the switch's channel is the **receiver** channel the firmware reads that aux channel from: the core's `rx_channel`
  (after the firmware's RX map) when given, else aux + 4 (ch5 = AUX1: iNav; RotorFlight's AUX1 is rc channel 6, which
  the RS7's map AECR1T23 reads from ch5). A range `linked` to another box (RotorFlight) follows that box, not the
  channel, and is not a position. RotorFlight's AND logic (`logic:"and"`): the box counts on a position only where
  all its AND ranges on that channel cover it, and its label names the other conditions — `ARM & ch8` (an AND slot on
  another channel), `RESCUE & ANGLE` (an AND slot linked to the ANGLE box);
- the window lists the switches as `ch6  [—] [ANGLE] [HORIZON]  [6]`: the pressed button follows the channel as sent;
  a click or the row's key (**switch 1..5**, defaults **5..9**, the N-th row in channel order, rebindable in Settings → Keys) moves it. The app
  drives a channel whose source is `ui` or `none`; a channel on the radio is shown (its position follows the radio),
  not driven. A switch the app starts driving (and every re-derivation: new modes, Settings closed) never lands on an
  ARM position by itself: it keeps an ARM position only when the user put it there (a click / key this session);
  otherwise it goes to the lowest position without ARM. A channel whose
  every position has ARM (one range over the whole span) is armed by the firmware whatever is sent;
- the header says where the modes came from: `modes: from firmware (t = 12.34 s)` or `modes: from bench snapshot
  (2026-10-01 14:02)` (before a core answers) — every `modes` message is saved into the bench (`modes`,
  informational; ≈120 bytes per box in the indented bench file, iNav's ~50 boxes ≈ 6 KB; unused slots dropped) and **replaces** its static `switches` (the
  pre-connect layout: one entry per derived switch; a hand-written note is kept in front, a switch on a channel the
  firmware has no range on goes, `switchOrder` is left as written) — logged as `bench switches: … → …` when it changes;
  `— no aux ranges` when the firmware
  has none. Before any modes (or with no ranges) only the bench's static switches have rows (the ARM / mode / third
  toggles of earlier versions are gone, 2026-10-23). The app subscribes to the modes (`subscribe {"modes":true}` after every `hello`): the
  core then reads them after every boot and replays the last read to a late client.
The **Status** window shows the active modes as text tags in its firmware line (`fc.mode`, else `state.fc.boxes`, the
permanent ids, named through the modes).

**OSD** (the Status sensors line): what the firmware's OSD is, from the core (`osd` in `hello` / `state`):
`OSD: MAX7456 30×16 (no video in FlightBench)` (analog OSD: the chip is modelled, there is no camera picture),
`OSD: MSP DisplayPort 50×18` (drawn in the OSD window / over the FPV camera), `OSD: none`. Nothing from a core that
does not say.

**OSD** window (Windows → OSD) and the OSD overlay over the FPV camera: the firmware's MSP DisplayPort canvas, drawn in
**the firmware's own symbol font** — the core's `osd` messages say which (`font`): RotorFlight's (256 glyphs, the
Betaflight layout) or iNav's (512 glyphs: its symbols sit where RotorFlight has other ones, and its HUD — the artificial
horizon, its sidebars, the crosshairs — is on the second page). Nothing to choose: the font follows the bench's
firmware; before the core knows the firmware (the first seconds of a boot) RotorFlight's is used, and a switch redraws
what is on screen at once. The picture updates only when something on it changes (iNav sends a frame only then), so a
still OSD is not a stuck one; a lost link shows NO SIGNAL over the dimmed last picture.

**Functions** window (Bench → Functions…, or Windows): the **environment functions** of the bench from the core's
manifest (`aircraft[].functions` of the bench's aircraft: *Calibrate accelerometer (6 positions)*, *Calibrate gyro*, *Launch*…):
one button each (`{"type":"function","name":…}`, while connected) with its **progress line** from the core's
`function` messages (`running… 0/6`, `step 3/6: left side down`, `done: …`, `FAILED: …`), and an **at first start**
checkbox that starts at the catalog's `auto_first_start` and is kept per bench (informational: the core decides the
first-start set from the aircraft). The buttons are off while a function runs (from its `start` / `step` until its
`done` / `failed`, or a new connection; a function the core replays to a late client shows its state again).
**Launch (L)** stays in the bottom bar.

**Config** window (Bench → Config…, or Windows; was *Firmware config*) — the developer's main tool for the firmware's
config (product owner 2026-10-02):
- **Script ↔ Firmware**: left the script the core sends, **by layer**, editable at the source (the same view as the
  wizard's summary: conflicts, edit a file at its line, disable / override a line, the bench lines box) — first the script
  this core started with, after an edit "the bench as it is now — not sent yet"; right the firmware's current `diff all`
  (layer 5: the firmware is its source): header — from the firmware / from the bench, **first start** / **saved
  snapshot** / **on request**, when, **stale** and why (armed, calibrating, in the CLI, busy, not ready) —, **Show cached**
  (`refresh:false`: the firmware untouched), **Refresh from firmware (reboots)…** (asks; off while armed or unknown),
  **Copy to clipboard**.
- **Changes** (diff against the script): what the configurator changed against the script — **changed** (script →
  firmware), **only in the firmware** (added), **only in the script** (removed / back at the default); **Selected → bench
  lines** / **All changes → bench lines** take the firmware's values into layer 5 (then Save bench lines), **Disable the
  script's line** for a removed one.
- **Operations** (explicit; the ones that reboot ask first and need the firmware disarmed): **Apply script now
  (reboots)…** (**full**: every line, not only what the firmware lacks), **Re-run <function>…** (each environment
  function), **Reset to first start…**, **Snapshot firmware config → bench lines…** (the core takes `diff all` — reboots —
  and writes it as the bench lines; the Config window and an open Edit bench show the new layer 5), **Export as preset…**
  (a folder: the bench JSON, its bench lines, its own copies of modified aircraft / board files, a README listing the
  layers). A refusal or result shows as one line under the operations.

**The core** (app/README.md → Core lifecycle): a bench in **local** core mode is checked (session-check) and
its core started by the app when the flight screen opens, stopped when the bench closes (unless kept running)
and on app exit; a **remote** bench is connect-only. Its output is in the Log / CLI window (`core:` lines). The
**Bench menu's Core group** shows and runs it: a read-only line `Core: running · pid 1234 · control 31250, msp 31235`
(or what the app's core is doing — checking, starting, exited with its code, rejected —; `Core: remote host:port
(connect only)` for a remote bench), **Restart core** (the app's own core only), **Bench check**, **Core address…**
(the host:port field: a non-loopback host makes the bench remote), **Use local core** (when offered), **Close bench**.
- **Remote bench on a machine with a local core**: never connected silently — the Bench menu offers **Use local
  core** (logged too; the connect goes ahead): it switches the bench to local, saves it and starts the local core.
  Bench → Edit bench… → Summary → Core switches either way.
- **Bench → Bench check**: runs the core's session-check on this bench and shows the report (the assembly,
  options, launch line, warnings / errors).
- **Bench → Restart core**: stop the app's core, check the bench again, start, connect. **Bench → Close bench** = back to the main menu (the core stops).
- **The firmware config is the bench's** (core branch `bench-state`; product owner's test 2026-10-01: every re-entry
  lost the calibrations and the modes set in the configurator): the app starts the core with a per-bench state file
  (`bench-state=`, next to the core's cfg-cache), the same file on every start of the same bench; the core saves the
  firmware's config there on every save and restores it at start. A core without the option is started without it (the
  log says so; the first-start snapshot is restored as before).
- **Bench → Edit bench…** opens the wizard **over the flight screen** (the core keeps running meanwhile). **Save &
  start** writes the bench and applies it in place: the core is stopped, the bench checked again and started with the
  same bench state — Status shows the delta (`config: applying 4 new lines (delta)…`), not a whole first start; the log
  and the windows stay. A new board / aircraft reopens the flight screen on the edited bench. **Cancel** changes nothing.
  Another name = a new bench (the original stays), with its own first start.
- **Bench → Reset firmware config to first start…** (asks; while connected): the core discards the bench's saved
  config and runs the whole first start again (script + first-start calibrations; the firmware reboots).
- **Rejected bench** (session-check exit 2): a dialog with the core's `session:` errors; nothing is started.
- **Port in use** (another core answers on the control port, a port cannot be bound, or the core exits with
  QEMU's bind error): "port 31250 is in use — another FlightBench core?", with **Use the next free ports**
  (control / msp / monitor / motion / pilot moved together, for this run) or **Connect to it** when a core answers
  (it greets with `hello`; another program on the port is named as such, no Connect to it).
- **Core crash** mid-flight: the Status link line `not connected (…) · core exited (code N)`, the Bench menu's Core
  line the same, the last 20 lines in the Log window, Bench → Restart core.
- **Log / CLI** window: the log (last 2000 lines) and a CLI line — Enter sends it to the firmware (`cli`; enters CLI
  mode, MSP clients cannot connect while in it). While the firmware is in its CLI the row says **in the CLI — MSP
  blocked**; **exit CLI (reboots)** (here and in Status) sends `{"type":"cli","exit":true}`: the core sends `exit`, the
  firmware leaves the CLI and reboots. The **Copy configurator address** tooltip says it too while the CLI blocks MSP.
  Empty / whitespace-only input sends nothing.
  Shell-like **history**: Up / Down recall older / newer commands; text typed before the first Up is a prefix — only
  commands starting with it come up (fish / zsh style, newest first, each command once) — and Down past the newest,
  or Escape, gives the typed text back (a second Escape leaves the field). The caret does not move on Up / Down;
  Left / Right / Home / End edit as usual. The history is global (all benches): `user://cli_history.txt`, one command
  per line, written at once on every sent command, consecutive repeats not stored, the last 1000 kept.
  **Ctrl+L** clears the log view (not the history). The field's placeholder and tooltip name the keys.

**The plane** (a bench whose aircraft has the plane FDM: the trainer, preset **Trainer · NAV LAUNCH**;
app/README.md → Plane):
- Bottom bar **Launch (L)** (+ **▾**: optional speed / duration / pitch overrides; unchecked = the scenario's
  launch, 50 km/h in 0.5 s at 10°): the hand / rail throw. Enabled while connected; not shown on a heli bench.
- **Aircraft** window: airspeed (m/s, km/h), α/β, thrust, prop rpm, throttle; aileron L/R, elevator, rudder in
  degrees; launch phase (`ramp…` / `released at …`), hold, scenario and its current step; a `mix_warning` row; a red
  **crash** banner with **Reset aircraft** (heli too).
- **First start** of a new firmware + script: the core configures the board and runs iNav's 6-position ACC
  calibration (~4 host minutes on the Windows desktop): Status shows `firmware: waiting…` and the config line
  `config: applying the CLI script…` → `config: calibrating the accelerometer… pose k/6 (label)` → `config: applied ·
  ACC calibrated` (later starts: `restored`), hidden once done; the Log the same with `config:`. Nothing times out; it
  is not an error.
- **Transmitter** window: once the core has read the firmware's modes, the switches derived from them (the
  trainer's `inav.cli`: ch5 `— | ARM`, ch6 `— | ANGLE`, ch7 `— | NAV LAUNCH`, keys **5 / 6 / 7**); the trainer's channels are all `auto`, so before
  that there is no row (only the order line, `switchOrder`). W / S the throttle.
- **Required order** to launch the trainer (iNav 9.1 NAV LAUNCH):
  1. **ANGLE** (ch6, key 6) and **NAV LAUNCH** (ch7, key 7) **on** — while disarmed: iNav enters launch mode only
     from a box switched on before arming;
  2. **ARM** (ch5, key 5) **with the throttle low** (the keys' throttle starts low; S lowers it);
  3. **throttle up** (W; ~70 %): iNav waits in launch mode, motor idle;
  4. **Launch** (L). iNav detects the throw (~70 ms into the ramp), starts the motor 0.5 s later and climbs at
     ~17°; turn NAV LAUNCH off (7) to fly on in ANGLE.

Semantics that matter:
- **Disconnect/connect battery** = power-cycle the MCU: it boots from what is in its flash, no
  diff re-applied (like the real board). **Restart bench** = erase the config sector, boot, apply
  the bench's diff as custom defaults — with the app's own core: the core is stopped and started again from
  the bench (a fresh session, checked first); with a remote core: `reset all` + `firmware`.
- **Replace firmware** (window "Firmware"): pick a new ELF/hex, or "watch file": when the file
  changes on disk (after `make`), the core reloads it, restarts the bench and reports in the log.
  Edit → build → test without leaving the app.
- **Signals**: everything that goes into or out of the flight controller is a named signal
  published by the core (servo/motor outputs, sensor inputs before and after noise, RC channels,
  MSP-visible values: attitude, PID outputs, rpm, battery, flags; CRSF telemetry; FDM state). The
  Signals window lists them; any signal can be plotted (multiple traces, shared time axis) or
  tabled, and recorded to CSV. Later: an emulated SPI flash so the firmware writes real blackbox
  logs readable by Blackbox Explorer.
- **Sounds**: beeper pin → tone; DShot beacon commands (< 48) → motor beep tone. Pilots know the
  arm/failsafe/error patterns by ear.

## Not now
A full aircraft editor (building the airframe). Cubes with axes are enough for the RS7.

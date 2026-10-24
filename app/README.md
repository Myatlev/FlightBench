# app/ — Godot .NET application

Godot **4.7.2 .NET (mono)** project, C#. State (2026-09-29): main menu with bundled presets and recent benches, bench
wizard (board / aircraft / wiring / board config / summary), settings (interface / UI scale, paths,
transmitter: sources, calibration, live channel bars, keys: every bench function with an editable bind),
flight screen: blueprint grid, four cameras
(Pad / Orbit / FPV / Chase), the aircraft's procedural visual from `models/<name>/visual/` (RS7: servos,
pushrods, swashplate, rotors animated from the core's state stream; the trainer plane: surfaces and prop;
placeholder boxes as fallback), **the plane** (Launch, `state.plane` in the Aircraft window, the first-start
config progress in Status, the crash banner, the switch layout; see **Plane** below),
embedded windows (Status, Aircraft, Transmitter, Log/CLI, Firmware, Signals, Wiring, Model, Faults, OSD, Functions,
Config; one fact in one place: see **Flight screen windows**) whose layout is saved into the bench together
with the camera. The device catalogue (with each device's ports), the board's
pads (printed labels, onboard devices), actuators and faults come from the core's `manifest`. Protocol: `docs/protocol.md`; UI concept: `docs/ui.md`.

This file is also the client's development log: the *Evidence* and *Checked* paragraphs cite screenshots. The ones under
`app/evidence/shots-2026-10-24/` were retaken on the v0.1.0 build (the paragraphs' numbers are from the original runs;
the set's README has each shot's recipe); the dated sets beside it carry the logs of their live checks.

## Run
```
# engine: Godot 4.7.2 .NET (mono) build — the standard, non-mono build cannot run C# projects
dotnet build app/FlightBench.csproj          # first time; the editor also builds on play
python tools/fake_core.py                    # stand-in core on :31250 (spool-up, lift-off, circle)
Godot_v4.7.2-stable_mono_win64.exe --path app  # then Connect (127.0.0.1:31250)
```
With a core executable (Settings → Paths → core, or `FB_CORE_EXE`, or the exported build's `data/core/`) the
app starts the real core itself for a local bench — see **Core lifecycle** below; `FB_CORE=127.0.0.1:31250`
keeps the old connect-only behaviour (e.g. against `fake_core.py`).
Environment knobs for scripted / headless runs (`--headless --quit-after N` prints the number of
states received, signal traces and samples, CSV rows and dropped commands at exit). All are read once
in `scripts/DevEnv.cs`; an on/off knob is on when set and not `0` / `false` / `off`. A run with a layout
knob (`FB_WINDOWS`, `FB_SIGNALS`, `FB_FAULT_SCRIPT`, `FB_CAMERA`, `FB_AXES`, `FB_MENU`, `FB_OSD`, `FB_OSD_OVERLAY`), a self-test, or `--headless`
never writes the bench (layout, last used); `FB_CORE` is never written into it either.

| variable | effect |
|---|---|
| `FB_BENCH=<name>` | start that bench from `user://benches/` at once |
| `FB_PRESET=<id\|name>` | main menu: open that bundled preset at once (file id `rs7-first-flight` or name `RS7 · First flight`) as the Presets list does: copy into the benches folder (or take the existing bench of that name), flight screen, local core |
| `FB_CORE=host:port` | connect to this core instead of the bench's; connect only (the app starts no core) |
| `FB_CORE_EXE=path` | the core executable (`qemu-system-arm`), used when there is no bundled `data/core/` and no Settings → Paths → core; also the core of the `corelaunch*` self-tests |
| `FB_BENCH_FILE=path` | a core session file used as is: the flight screen checks and starts it instead of the bench's own session (this run); the bench of the `corelaunch*` self-tests |
| `FB_SCREENSHOT_CONFIG=path.png` | flight screen: one screenshot while the first-start ACC calibration runs (at its third pose), for the config-progress evidence; the run goes on |
| `FB_SCREENSHOT_ALT=<m>` | with `FB_SCREENSHOT`: flight screen, the screenshot once the aircraft is that high (lift-off evidence), then the app quits (and stops its core) |
| `FB_SIGNALS=id,id,…` | check these signals in the Signals window (state ids like `euler_deg.roll`, or core ids → `subscribe`) |
| `FB_CSV=path` | record the checked signals to this CSV from the first state |
| `FB_SCREENSHOT=path.png` (+ `FB_SCREENSHOT_FRAME`, default 300) | save the viewport at that frame (windowed run) |
| `FB_SCREEN=wizard\|settings` | open that screen on the first frame (screenshots without clicking) |
| `FB_WIZARD_STEP=n` | with `FB_SCREEN=wizard`: jump to step n (1-based) |
| `FB_WIZARD_EDIT=<name>` | with `FB_SCREEN=wizard`: the wizard edits that bench (edit mode) |
| `FB_WIZARD_SCROLL=<px>`, `FB_WIZARD_LINE=<text>` | with `FB_SCREEN=wizard` + `FB_WIZARD_STEP=5`: once the session-check answered, the page scrolled down by px and the first line of the layered view containing the text selected (screenshots) |
| `FB_CLI_SEND=<line>` | flight screen: once connected (20 states in) send that CLI line — the firmware enters its CLI (the CLI-indicator screenshot); the run does not write the bench |
| `FB_CONFIG=script\|changes` | flight screen: once connected (50 states in) the Config window opens (40,60 1500×800) in that mode; `changes` asks the core for the diff against the script (screenshots) |
| `FB_DIALOG=delete\|reset\|edit\|apply\|snapshot` | flight screen: after 30 frames open Bench → Delete bench… / Reset firmware config… (never confirmed) / Edit bench… (the wizard over the screen) / the Config window's Apply script now / Snapshot confirmation; main menu: `delete` opens the Delete… confirmation of the selected bench; for screenshots, the run does not write the bench |
| `FB_EDIT_RESTART=1` | flight screen, a live check of Edit bench → Save & start against the core the app started: once the firmware answers, the bench is applied again as edited (same name) — the core stopped, checked, started with the same bench state; once the firmware answers on the new core one line `[edit-restart] PASS/FAIL: …` (pids, ports) and quit; 4 min at most |
| `FB_BENCH_DIR=path` | benches folder instead of `user://benches/` (screenshot runs on throw-away benches) |
| `FB_MENU=<title>` | flight screen: open that top-bar menu (e.g. `Bench`) after 30 frames, for screenshots; the run does not write the bench |
| `FB_WIZARD_AUTOFILL=1` | with `FB_SCREEN=wizard`: go to step 3 and replace the wiring with the board-config autofill (screenshot of a filled graph) |
| `FB_WIRING_SELFTEST=1` | with `FB_SCREEN=wizard`: the `wiring` self-test below, on the wizard's graph (`[selftest] … PASS/FAIL`) |
| `FB_SELFTEST=all` (or a list: `rcmath,cfg_address,normalize,settings,bench,bench_edit,tx_model,bench_signals,signals,osd,presets,cli_history,corelaunch,corelaunch_restart,corelaunch_reject,corelaunch_portbusy,core,wiring,wizard_script_view,keys,keys_hint,radio,plane_state,plane_visual,launch_cmd,config_progress,modes_switches,functions_panel,topbar,osd_line,diff_view,osd_font,status_compact,aircraft_window,transmitter_owns_switches,core_menu,layout_migration,core_fixtures,bench_delete,cli_indicator,edit_in_place,layers_view,bench_lines,config_changes,snapshot_export,boot_hint`; the four plane tests: see **Plane**; `modes_switches`..`diff_view`, `osd_font`: see **Firmware views** / **OSD**; `status_compact`..`core_fixtures`: see **Flight screen windows**; `bench_delete`, `wizard_script_view`, `cli_indicator`, `edit_in_place`: see **Bench state, edit in place, delete**; `layers_view`, `bench_lines`, `config_changes`, `snapshot_export`, `boot_hint`: see **Config layers**) | headless self-tests, each judged `[selftest] <name>: PASS/FAIL`, then a summary; quits by itself with the failure count as exit code. `corelaunch*` (see **Core lifecycle**) need `FB_CORE_EXE` (without it: PASS "skipped: no core exe") and `FB_BENCH_FILE`, and use ports 31290..31294 (`FB_CORE_PORTS` moves them, e.g. `31295`); `core` needs a local core (`FB_CORE` or 127.0.0.1:31250: `python tools/fake_core.py`; it sends `cli` lines the fake core echoes); `keys` runs the flight screen's keys test with an in-memory bench (also the keyboard layer: every stick key through the bind table moves its channel and lets go on release; `stick_collective_plus` rebound from W to I: I raises the collective, W does nothing; a stick key pressed while the CLI line has the focus moves nothing) and reports `keys_hint` (the Transmitter window's hint line equals `Keybinds.Hint`, the rebind in it); `radio` (same screen) plugs a test joypad through `IJoyReader` holding ch1 at 1800 and ch3 at 1700, presses → (ch1 2000 `kb`), releases it (1500 `kb`, the pad at rest does not take it back), moves the pad's axis (ch1 1600 `joy`), holds W 0.1 s (ch3 1700 → 1750 `kb`: the integrator continues from the pad's value), moves the pad's collective (1500 `joy`), unplugs it (channels must hold, `fault crsf link_lost on` sent, Faults row on) and plugs it back (`on:false`, the pad's values again); `settings` also checks that an older file's `keyboard` source reads as `none`; `bench_signals` checks signals in a Signals panel → bench JSON → re-checked in a new panel; `osd` feeds `OsdBuffer` a clear + cells, a partial update, attrs (blink 0x40 counted; page bits + 0x80 stored raw, not blink, page logged once; blink phase from the cell's write, restarted by a re-send), `link:false` → flag set with the canvas kept, next frame → cleared, `\u00NN` codes decoded to glyph indices, an overlong string (clipped), a resize, and checks the grid text and the fit rect, then the symbol font: 256 glyphs, 'A' has white pixels, 0x00 and space blank, battery/crosshair glyphs present, cached, a decoded 0x96 maps to its atlas cell with the font's ink, one-page atlas, missing file → null, bad header refused; then iNav's (see **OSD**): its font has 512 glyphs (atlas 16 × 32), RotorFlight's stays 256, the glyph index by font, the page-1 HUD glyphs (0x131 sidebars, 0x13A / 0x166 / 0x13B crosshairs, 0x14C..0x154 horizon) have ink and differ from page 0's glyph of the same byte, iNav's page-0 symbols (0x63 battery, 0x6A amp, 0x76 alt m, 0x08 sats, 0x1F volt), the core's captured iNav canvas (`app/tests/osd-inav-boot-canvas.txt`) rebuilt cell for cell from one full frame, deltas after gaps in `frame` (+5, +31 without `font`, back to +18, none) applied, a font switch keeps the canvas and changes the render, `Reset` (`FB_OSD_PNG=<folder>`: also dumps `osd_symbols_row.png` — a row of symbol codes —, `osd_atlas.png`, `osd_inav_canvas.png` — the captured canvas in iNav's font — and `osd_inav_atlas.png`); `presets` (see **Presets**) parses every `<data root>/presets/*.json` as a bench, checks its firmware / board config / profile paths are relative and present under the data root (and that the wizard's status line finds them with another working directory), writes its session (paths kept relative) and, with `FB_CORE_EXE`, runs the core's `session-check` on it (exit 0, `launch:` line, the core's `firmware:` / `board_config:` / `profile:` under the data root, `msp_osd` on uart4 for the OSD preset; else "session-check skipped: no core exe"), then opens the first preset into a temp benches folder (copied; a second open takes the existing bench with the user's change) and checks the bundle is byte-identical; `cli_history` works on a temp file under `user://` (never `user://cli_history.txt`): appends (on disk at once, no BOM), consecutive duplicates and blank lines not stored, prefix recall order (newest first, each command once, the typed text itself not a match), Down past the newest and Cancel give the typed text back, a reload of the class reads the same entries, a file over the 1000 cap is compacted on load (and again after an Add over the cap), then drives a `CliLine` with key events through the root viewport: Up recalls with the caret at the end, Up at the oldest match leaves text and caret alone, Left / Home still move the caret, Down past the newest, Escape while browsing (still editing), Enter sends and stores, whitespace-only sends nothing, Ctrl+L calls the log clear and keeps the history; `bench` also round-trips the `core` mode (remote; local keeping its remote host) and checks the migration of benches without it (loopback / empty → `local`, other hosts → `remote`, an explicit value wins) and the Use-local-core banner rule; `tx_model` (see **Transmitter**: the bench's `transmitter` block round trip and normalisation, the migration of a bench with the old `switches` and without the block, the wizard's new-bench default per the board's `firmware_family`, the switch2 / switch3 / switch6 values and ArduPilot's read-back thresholds, the rows in channel order, the presets' blocks); `bench_edit` saves as / edits / renames benches in a temp folder under `user://` and checks the original stays byte-identical, and that old bench files without `core` load as remote / local by their host and are written with the field; `wiring` drives the wizard graph's handlers: onboard devices pre-wired, ESC `tlm`/`rpm` wired by port, bench JSON round trip (wires by device.port → pad, fields, labels, a saved position), the new-form session, refusals (receiver on the RX-only TELEM, wrong kind, onto the onboard receiver's pad), an I2C bus shared, a detachable onboard device detached (→ `detached` session entry, inspector warning) and put back, a soldered one refused, a user device onto an on-board pad (refused, accepted once detached), the two board nodes, old `slot` benches migrated as the core reads them (the smoke bench, gyro only, from a temp bench folder: gyro + the onboard devices the old-form rule keeps + assumed ESC/servos, new form once saved; an omitted detachable onboard device such as the baro is detached), `msp_osd` in the catalogue with its in/out UART port (refused on TELEM, accepted on PORT-A), the Faults window gated by it, the Summary page's Core row (local by default with host / port greyed and what local resolves to; remote as a click picks it, host / port editable, into the bench JSON; back to local keeps the host; the old smoke bench with `coreHost` 192.168.1.10 → `remote`) — expectations are derived from the manifest, and a fixed set (smoke = 8 wires, 5 assumed, `dps310` detached; onboard detachable crsf/dps310/icm42688p, soldered spiflash; 9 devices) is asserted on the fixture. **Manifest in self-test runs:** `FB_MANIFEST`, else the bundled fixture `app/tests/manifest-fixture.json` — never the user's `user://manifest.json` cache; a live manifest is ignored during the run and not cached. Refresh the fixture from the current core: `ssh <homelab> "cd ~/flightbench && core/qemu/run.sh --manifest" > app/tests/manifest-fixture.json`, run `FB_SELFTEST=all`, update the fixed numbers in `Wizard.WiringSelfTest`. `FB_SELFTEST=reconnect` (not in `all`): bare connect → disconnect → connect → disconnect to `FB_CORE`, any core, nothing sent |
| `FB_UI_SCALE=<number\|auto>` | override the saved UI scale for this run |
| `FB_SETTINGS_TAB=<title>` | with `FB_SCREEN=settings`: open that tab (e.g. `Transmitter`) |
| `FB_FAKE_JOY=1` | fake joystick on device 0: axes 0..7 slow sines, button 0 toggling every 2 s (fed through `Input.ParseInputEvent`); also logs every 50th `rc` send (ch1..8); `FB_FAKE_JOY=0` = off |
| `FB_RC_LOG=1` | log every 50th `rc` send (ch1..8) without the fake joystick |
| `FB_WINDOWS=all\|Name,Name` | open these flight-screen windows regardless of the bench layout (`Status`, `Aircraft`, `Transmitter`, `Log / CLI`, `Firmware`, `Signals`, `Wiring`, `Model`, `Faults`, `OSD`, `Functions`, `Config`); this run only |
| `FB_OSD=1` | open the OSD window (same as adding `OSD` to `FB_WINDOWS`); this run only |
| `FB_OSD_OVERLAY=1` | OSD overlay over the FPV camera on for this run (a bench with it off keeps it off on disk); with `FB_CAMERA=fpv` for goggles-view screenshots |
| `FB_OSD_PNG=<folder>` | with the `osd` self-test: dump a row of symbol glyphs, both font atlases and the captured iNav canvas as PNGs there |
| `FB_CLI_HISTORY=path` | the Log / CLI history file instead of `user://cli_history.txt` (a self-test run without it keeps the history in memory: the real file is never read or written) |
| `FB_MANIFEST=path` | offline manifest (e.g. saved `core/qemu/run.sh --manifest` output) instead of the cached one, until a live one arrives |
| `FB_WIZARD_SELECT=<device>` | with `FB_SCREEN=wizard`: select that device's node (inspector screenshots) |
| `FB_SETTINGS_SCROLL=end` | with `FB_SETTINGS_TAB=Keys`: scroll the table to the fault rows |
| `FB_RC_OFF=1` | never send `rc` (connecting to a core someone else is flying) |
| `FB_KEYS_SELFTEST=1` | flight screen: print the manifest source and counts, bind body axes to Ctrl+F9 and a fault with params to Ctrl+F10 through the table, feed synthetic key presses, print the effect and the `fault` lines sent, then the stick keys / rebind / text-focus / hint checks (`[keys-selftest] … PASS/FAIL`); binds restored, nothing saved |
| `FB_FAULT_SCRIPT=1` | flight screen, short live check: `crsf.lq` on (lq 30, rssi −95) → `FB_SCREENSHOT` while it is lit → off → `crsf.freeze` on → off, logging `state.faults`; quits by itself once all are off |
| `FB_CORE_PORTS=control[,msp,monitor,motion,pilot]` | the five ports of the core this run starts (one number = control, the rest +1..+4), e.g. `31290` for a check beside a core already on the defaults; not saved in the bench |
| `FB_LIVE_CHECK=1` | flight screen, the windows against a real core: once the firmware answers disarmed, its modes came by subscription and any first start is over → `FB_SCREENSHOT` of the layout; then Functions + Config opened, Show cached, else **Refresh from firmware** (reboots the firmware) until a real diff shows → `<FB_SCREENSHOT>-views.png` and one `[live-check] PASS: …` line (Transmitter header + switches, Functions, diff header, Status, Aircraft rows, configurator address, Core line); then on a heli bench the **take-off from the keyboard** (see **Transmitter → Live check: take-off**): key switch_1 → `armed` event, key switch_2 → head ≥ 1600 rpm (`<FB_SCREENSHOT>-spool.png`), W held → altitude ≥ 0.5 m → `[live-check] PASS: take-off …` and `<FB_SCREENSHOT>-airborne.png`, or FAIL with the reason and the last state (exit code 1); with `FB_SCREENSHOT`, Settings opened over the bench → `<FB_SCREENSHOT>-settings-keys.png` and `-settings-transmitter.png`; quits; 8 min for the views + 4 for the take-off; the run does not write the bench |
| `FB_NO_JOY=1` | no joypad is read this run (`Joy.Reader` = an empty reader), as on a PC without a pad: the keys are the only thing moving channels 1..4 — for deterministic runs (a pad's noise cannot take a channel). Not needed for the keyboard: the keys are a layer beside the joypad (the live check passes with and without it) |
| `FB_CAMERA=pad\|orbit\|fpv\|chase` | start with this camera (this run only, not saved into the bench) |
| `FB_ORBIT=dist[,yaw_deg[,pitch_deg]]` | initial orbit camera (default 8 m, 34°, 20°; yaw 0 = behind the pad, 90 = east); e.g. `0.75,115,15` = close-up of the head and servos |
| `FB_AXES=1` | start with the body axes + actuator labels shown (View → Body axes and X still toggle them; this run only) |
| `FB_ACT_SWEEP=1` | feed the aircraft visual slow actuator sines (swash cyclic circle + collective, tail) instead of the core's actuators — to see the linkage move against the fake core |

Smoke test and self-tests against the stand-in core (+ the packaged Windows core for `corelaunch*`):
```
python tools/fake_core.py
FB_BENCH=smoke FB_CORE=127.0.0.1:31250 FB_SIGNALS=euler_deg.roll,rpm,fdm.alt_m FB_CSV=out.csv   Godot_v4.7.2-stable_mono_win64_console.exe --path app --headless --quit-after 500
FB_SELFTEST=all FB_CORE=127.0.0.1:31250 FB_CORE_EXE=<core dir>/qemu-system-arm.exe FB_BENCH_FILE=<an RS7 bench .json>   Godot_v4.7.2-stable_mono_win64_console.exe --path app --headless
```
2026-09-29: 17 PASS, 0 FAIL with `cli_history` (Windows, packaged core + `fake_core.py`, `FB_BENCH_DIR` = a temp folder); `presets` also
PASS in the exported layout (`dist/FlightBench` copied to `%TEMP%`, empty `APPDATA`, `FB_CORE_EXE` = its `data/core`).
2026-09-29 (branch `app-plane`): **21 PASS, 0 FAIL** — the 17 + `plane_state`, `plane_visual`, `launch_cmd`, `config_progress`
(same command; the packaged core has the `speedybee-f405-wing` machine, so `presets` runs the trainer's session-check too).
2026-09-29 (branch `fix-restart`): **22 PASS, 0 FAIL** — the 21 + `corelaunch_restart` (same command).
2026-09-29 (branch `app-bench`): **38 PASS, 0 FAIL** — the 34 of `main` + `wizard_script_view`, `bench_delete`,
`cli_indicator`, `edit_in_place` (fake core on 31260, `FB_CORE_PORTS=31295` for `corelaunch*`, `APPDATA`,
`FB_BENCH_DIR` and `XDG_CACHE_HOME` = temp folders; the packaged core for `corelaunch*`, `presets`, `wizard_script_view`).
2026-09-30 (branch `app-osd-font`): **39 PASS, 0 FAIL** — the 38 of `main` + `osd_font` (same command as `app-bench`: fake
core on 31260, `FB_CORE_PORTS=31295`, the packaged core of 2026-09-29 for `corelaunch*`, `presets`, `wizard_script_view`;
`APPDATA`, `FB_BENCH_DIR`, `XDG_CACHE_HOME` = temp folders). The iNav font is checked against the core's captured canvas
(core branch `inav-osd`), not against a live iNav core (the packaged core predates it).
2026-09-29 (branch `app-modes`): **28 PASS, 0 FAIL** — the 22 + `cfg_address` (from `fix-msp-inav`) + `modes_switches`,
`functions_panel`, `topbar`, `osd_line`, `diff_view` (same command with the fake core on 31260 and `FB_CORE`, `APPDATA` and
`FB_BENCH_DIR` = temp folders). The five run against `fake_core.py`'s `modes` / `diff` / `function` / `osd` (the shapes of
the core branch `core-modes`, not yet merged): not yet checked against the real core.
2026-09-30 (branch `app-reconcile`, `main` b6b1d5c merged: config-layers + inav-osd + bench-state all in the core):
**44 PASS, 0 FAIL** — app-layers' + app-osd-font's self-tests together, both reconciled against docs/protocol.md and
the rebuilt `dist-core\qemu-system-arm.exe` (2026-09-30, `.rebuilt` marker; same command as `app-osd-font` above).
The packaged core now reports full layered provenance (`wizard_script_view`: "154 CLI lines, layers from the core" —
no inference) and models `msp_osd` on the speedybee-f405-wing (`presets`). **Live-checked beyond the self-tests**
(this session, not scripted): the "Trainer · FPV with OSD" preset with its own `bench_cli` and a `disabled_lines`
entry, started against `dist-core` on ports 31295-31299 — the layered wizard view shows real `config_src` provenance,
the disabled device line and the winning bench line both reach the core (`app/evidence/shots-2026-10-24/config-layers-live-trainer-osd.png`);
the same bench, live, renders iNav's OSD font once the firmware answers (`app/evidence/shots-2026-10-24/osd-inav-live-trainer.png`) —
the iNav font is no longer only checked against the core's captured canvas.
2026-10-23 (branch `tx-model`): **45 PASS, 0 FAIL** — `main`'s 44 + `tx_model` (fake core on 31260, `FB_CORE_PORTS=31290`,
the packaged `dist-core` for `corelaunch*` / `presets`, `APPDATA` / `FB_BENCH_DIR` / `XDG_CACHE_HOME` = temp folders); the
live-check take-off on the exported build: `app/evidence/tx-model-2026-10-23/`.
2026-10-23 (branch `kb-layer`): **46 PASS, 0 FAIL** — `main`'s 45 + `keys_hint` (same command); the live-check take-off passes
with the Xbox pad plugged in and with `FB_NO_JOY=1`: `app/evidence/kb-layer-2026-10-23/`.
`python tools/fake_core.py --plane` is a stand-in for the trainer (a compressed first start with the config and
calibration events, the hand launch on `launch`, `state.plane`) for UI work without the 4-minute first start.
Both fake cores also send the firmware views (`modes` after `hello` and on request, `diff`, `function` progress,
`hello.osd`, `state.fc.boxes`; see the script's docstring); `--manifest app/tests/manifest-fixture.json` adds a
manifest with a `functions` catalog (the app caches a live manifest into `user://manifest.json`: use an empty `APPDATA`). Not for the self-tests:
`FB_SELFTEST` wants the plain `fake_core.py` (`core` needs its `cli` echo; the plane screen tests inject their own
events and states, and the plane stream would interleave with them).

Against the real core: the app starts it (Core lifecycle). A core on another machine:
`core/qemu/run.sh --session core/qemu/sessions/rs7.json` there (`CTL_HOST=0.0.0.0`), the bench's core address
= that host (connect only), or `FB_BENCH=<bench> FB_CORE=host:31250`; checked on the homelab with the Linux mono
build (smoke log of 2026-09-25: 103 states in 400 frames).

## Core lifecycle
The app starts, configures and stops the core itself (`scripts/CoreLauncher.cs`, `scripts/Flight.Core.cs`),
following `docs/protocol.md` → "Launching the core".
- **Which core**: `<exe dir>/data/core/qemu-system-arm.exe` (the exported build, `tools/export.ps1`), else
  Settings → Paths → **core** (`CorePath` in `settings.json`; empty = auto), else `FB_CORE_EXE`. The Paths tab
  shows what auto found. The **data root** (the core's `root=`, its working directory): `<exe dir>/data` in an
  exported build that has it, else the repo root (`RepoRoot`, the one the wizard reads `boards/` / `models/` from).
- **When**: the bench says which core it runs on — `core` in the bench JSON, the wizard's Summary → **Core**
  row (also reached from the flight screen's Bench → Edit bench…): **Local (bundled)** (`"core": "local"`, the
  default for new benches) = the app starts the core found above on the bench's ports (`corePort` / `mspPort`) and
  connects to `127.0.0.1`; **Remote host:port** (`"core": "remote"`) = connect only to `coreHost:corePort` (the
  homelab over tailnet), nothing started. The row shows what the local core resolves to (path and source), or
  "no core found — set it in Settings → Paths". A local bench keeps the `coreHost` it remembers, unused.
  `FB_CORE` or a self-test run: connect only. Local with no executable: connect only, logged.
- **Migration**: a bench without `core` (written before the field) gets it on load from the address it used to
  connect to: `coreHost` loopback (`127.0.0.1`, `localhost`, `::1`) or empty → `local`, any other host →
  `remote` (so an old remote bench keeps connecting where it did); an unknown value is migrated the same way.
  The field is written on the next save. Typing a non-loopback address in **Bench → Core address…** switches the
  bench to `remote` (logged; Restart core is greyed, Use local core is offered); a loopback address typed on a
  remote bench does not switch it back (typing only connects) — Use local core does.
- **The Bench menu's Core group** (product decision 2026-10-01: the core is not the Status window's): a read-only
  line `Core: running · pid 1234 · control 31250, msp 31235` (or what the app's core is doing — checking, starting,
  exited (code N), rejected —, `Core: remote host:port (connect only)`, `Core: host:port (FB_CORE: connect only)`),
  **Restart core** (the app's own core only), **Bench check**, **Core address…** (host:port dialog), **Use local
  core** (when offered, below), **Close bench**. The menu is rebuilt each time it opens. Status mentions the core
  only while it matters to the link (`not connected (127.0.0.1:31250) · core exited (code 1)`).
- **Remote bench, local core available**: never connected silently. **Bench → Use local core** is offered and the
  log says so (the connect goes ahead); it switches the bench to `local`, saves it (in a run that may write the
  bench) and starts the local core. Without a local core (or under `FB_CORE`, or in a self-test run) it is not offered.
- **Session file**: the bench → `user://sessions/<bench>.session.json` (`BenchData.ToSession` with the data root;
  UTF-8 without BOM; rewritten on every start; `<bench dir>/sessions/` when `FB_BENCH_DIR` moves the benches, so dev /
  test runs leave nothing in the real `user://sessions`). `FB_BENCH_FILE` replaces it with a given file. A fresh install
  (no manifest cached, built-in catalogue) first reads the manifest from the core (`session-check=on` without a
  bench) so the session's wiring is typed as the core expects; `DevHooks` does the same at start-up for the wizard.
- **Validate**: `qemu-system-arm -M nexus-xr,session=<file>,root=<root>,session-check=on -display none`; the app
  reads the `launch:` line (`icount_shift`, `control`, `msp`; unknown keys ignored, parsing stops at `manifest:`;
  an older report's `icount:` / `ports:` lines as fallback) and logs `bench check: ...` plus the `session:` notes.
  Exit 2 → **Bench rejected** dialog with the `session:` lines, nothing started. **Bench → Bench check** runs the
  same check any time and shows the whole report (manifest line shortened).
- **Bench state** (core branch `bench-state`, being built on the homelab; see **Bench state, edit in place, delete**):
  the start adds `bench-state=<cfg-cache>/<bench-id>.state.bin` to `-M` (`<bench dir>/state/` under `FB_BENCH_DIR`),
  the same file for every start of the same bench, when the core's `-M <machine>,help` lists the option; an older core
  (the packaged main of 2026-09-29) is started without it and the log says so.
- **Start**: the contract's command line (`-M nexus-xr,session=...,root=...,ctl=127.0.0.1:<control>[,bench-state=<file>] -nographic
  -icount shift=<S>,sleep=off -chardev ...u1 (msp) -serial chardev:u1 -monitor tcp:127.0.0.1:<monitor> -chardev
  ...fbmotion -chardev ...fbpilot`), working directory = data root; paths with `/`, commas doubled. Ports: control
  and msp from the bench (`corePort` / `mspPort` → `ports`), monitor / motion / pilot = **31236 / 31237 / 31238**
  (the bench schema has no fields for them: fixed defaults, moved only by the port-in-use offer below). stdout and
  stderr are read line by line (both pipes, asynchronously) into the **Log / CLI** window as `core:` lines. The
  app polls the control port (20 s; on timeout the core is stopped and the error shows its last lines), then
  connects as before.
- **Port in use**: a core already answering on the control port, a port that cannot be bound, or the core
  exiting at once with QEMU's bind error → "port 31250 is in use — another FlightBench core?"; the dialog offers
  **Use the next free ports** (all five moved by the same offset, for this run; the bench keeps its ports) and,
  when a core answers, **Connect to it** (not owned: never stopped by the app). "A core answers" = something
  accepts on the control port and sends a `hello` among its first JSON lines (1.5 s); any other listener there is
  reported as "port N is in use — by another program" (no Connect to it).
- **Stop / restart**: **Restart bench** (F5), **Replace firmware & restart** and **Bench → Restart core** stop the
  app's core and start the bench again (check included). Leaving the
  flight screen (**< Menu**, **Bench → Close bench**, Edit bench) stops it — TerminateProcess on Windows, SIGTERM
  elsewhere — unless Settings → Paths → **Keep the core running after closing the bench** is on (off by default;
  the kept core is reused when the same bench with the same session opens again). App exit stops every core the
  app started (DevHooks), whatever that setting; on Windows the cores are also in a kill-on-close job object, so
  a killed app leaves none behind. A core the app did not start is never stopped.
- **Crash**: the core exiting mid-flight → the Status link line `not connected (…) · core exited (code N)`, the Bench
  menu's Core line the same, the last 20 lines of its output repeated in the Log window (opened), **Bench → Restart core**.
- **Self-tests** (`FB_SELFTEST`, need `FB_CORE_EXE`; without it each is PASS "skipped: no core exe"; ports
  31290..31294 so a core on the defaults is not disturbed): `corelaunch` — `FB_BENCH_FILE` checked (launch line:
  shift 2, ports 31250/31235), started, control port waited for, `hello` + `manifest` received, stopped, the pid
  gone; `corelaunch_restart` — the Restart bench path: `FB_BENCH_FILE` started, its output drained with a cursor,
  then relaunched with that core as the one to stop first (stop → check → start on the same ports; a CLI-configured
  bench restores its config snapshot from `cfg-cache`): Started (not "port in use"), the first core gone, the first
  core's cursor read on the new one without an exception (from its first line), `hello`, stopped; `corelaunch_reject` — a temp bench with the receiver on the RX-only TELEM pad → exit 2, the `session:`
  error captured, nothing spawned; `corelaunch_portbusy` — a TCP listener on 31290 first → "port 31290 is in use",
  not taken for a core, nothing spawned; QEMU's bind-failure lines map to the port they name; the hello probe says
  false for a silent listener and true for one that greets with `log` + `hello` (`corelaunch` also probes the
  real core: true).

## Export (standalone Windows build; macOS below)
Presets in `app/export_presets.cfg`: **Windows Desktop** (release: `dist/FlightBench/`, PCK embedded in the exe,
no .NET debug symbols, no console wrapper) and **Windows Desktop (debug)** (`dist/FlightBench-debug/`, .NET
debug symbols, `FlightBench.console.exe` wrapper). `include_filter` packs the non-resource files the app reads
from `res://` — `*.mcm` (OSD fonts), `assets/osd/NOTICE` and `assets/osd/FONTS.md`; anything new read via `FileAccess` from `res://`
must be added there, or it is missing from the build (the editor run does not notice). `dist/` is git-ignored.

Export templates (once per machine, ~1.2 GB): `%APPDATA%\Godot\export_templates\4.7.2.stable.mono\` must
hold the unpacked `templates/` of
`https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_mono_export_templates.tpz`
(a zip: `unzip x.tpz`, move `templates/*` into that folder; or Editor → Manage Export Templates → Download).
```
# from the repo root: export + data/ (the script calls Godot's --export-release / --export-debug itself)
powershell -ExecutionPolicy Bypass -File app/tools/export.ps1           # release -> dist/FlightBench/
powershell -ExecutionPolicy Bypass -File app/tools/export.ps1 -Debug    # debug   -> dist/FlightBench-debug/
powershell -ExecutionPolicy Bypass -File app/tools/export.ps1 -DataOnly # only refresh <dist>/data/
powershell -ExecutionPolicy Bypass -File app/tools/export.ps1 -Presets 'rs7-first-flight'   # release profile: these presets + their firmware only
```
`-Presets` (names without `.json`, wildcards) is the release profile: `data/presets/` gets the matching presets and
`data/firmware/` only the files a kept preset names (plus `FIRMWARE.md`); the script prints what it left out
(v0.1 ships `rs7-first-flight` alone: the stock RotorFlight hex, official firmware only; boards/models stay whole, they are
small and the wizard can build any bench from them with the user's own firmware). `app/evidence/` (screenshots)
carries a `.gdignore`, so Godot neither imports nor packs it.
```
# Godot: -Godot <console exe>, else $env:GODOT, else `godot` on PATH (the 4.7.2 .NET console exe)
```
### macOS: `app/tools/export.sh`
Preset **macOS** in `export_presets.cfg` (universal binary — the 4.7.2 template ships only `godot_macos_release.universal`,
so `arm64` alone is refused; `rendering/textures/vram_compression/import_etc2_astc=true` in `project.godot` is required
for it). `app/tools/export.sh` is the counterpart of `export.ps1`: Godot export → `dist/FlightBench-macos/FlightBench.app`,
the same `data/` rules into `Contents/Resources/data/`, the core from `dist/core` (`core/qemu/tools/package-mac.sh`) into
`Contents/Helpers/qemu-system-arm` + `Contents/Frameworks/*.dylib`, `LICENSE` + `README.txt` (`tools/dist-README-macos.txt`)
next to the bundle; `--presets`, `--data-only`, `--zip`, `--core-dir`, `--godot` as in the PowerShell script. `CoreLocator`
and `AppSettings.DefaultRepoRoot` know both layouts (`<exe dir>/data` and the bundle's). Godot's .NET export runs
`/usr/local/share/dotnet/dotnet` when it exists (`GodotTools.DotNetFinder`), so the .NET 8 SDK must be installed there
(`brew install --cask dotnet-sdk@8`), a Homebrew `dotnet@8` on `PATH` is not enough.

Signing is inside-out and explicit (Godot's own signing is off in the preset, because files are added after the export):
every Mach-O in the bundle (the .NET runtime's dylibs sit in `Contents/Resources/data_*`, where `codesign --deep` would
not find them), the core with `core/qemu/tools/qemu-entitlements.plist` (JIT: QEMU's TCG), the app with
`tools/macos-entitlements.plist` (JIT: CoreCLR). Without `--sign` the bundle is ad hoc and **without** the hardened
runtime (a hardened ad-hoc core cannot load its own dylibs: library validation wants a Team ID); it runs on the
building Mac only. `--sign "Developer ID Application: Name (TEAM)"` signs with the hardened runtime and a timestamp;
`--notarize <profile>` (a `xcrun notarytool store-credentials` keychain profile) submits, waits, staples and asks
Gatekeeper (`spctl --assess`). Check: `FB_PRESET=rs7-first-flight FB_LIVE_CHECK=1 dist/FlightBench-macos/FlightBench.app/Contents/MacOS/FlightBench`.
Result (2026-10-24, M-series Mac): universal app 322 MB (two .NET runtimes) + arm64 core 32 MB → 355 MB, zip 129 MB;
live-check 2 PASS. Self-tests run the same way (`FB_SELFTEST=all FB_CORE=127.0.0.1:31250 <Godot_mono.app>/Contents/MacOS/Godot --path app --headless`
with `python3 tools/fake_core.py`): 46/46.

Result (2026-09-26): `FlightBench.exe` (~105 MB, engine + PCK) + `data_FlightBench_windows_x86_64/` (the .NET
runtime + `FlightBench.dll`, 186 files) + `data/` (8 files, ~24 KB) = ~182 MB; ship the folder as a whole. The
exe runs the same `FB_*` dev knobs (`FB_BENCH=smoke FB_CORE=127.0.0.1:31250 dist/FlightBench/FlightBench.exe
--headless --quit-after 300`).

**Repo data in the build (`data/`).** Godot packs only `res://` (`app/`); `include_filter` cannot reach
`../boards` or `../models`, so `tools/export.ps1` copies, after the export, what the app reads from them into
`<dist>/data/` (rebuilt from scratch each run): `boards/<id>/*.config`, `board.json`, `README.md`;
`models/<id>/aircraft.json`, `README.md`, `visual/*.tscn`, `**/*profile*.txt`; `presets/*.json` + `README.md`;
`firmware/**` (the firmware the presets fly + `FIRMWARE.md`, its source and licence). The script lists what it
bundled, fails when a preset names a path that is not in `data/`, and prints the data and build sizes. Anything
new the app reads from `boards/`, `models/` or `presets/` must be added to the script.

```
data/                       the data root (the core's root=, its working directory)
  boards/nexus-xr/          NEXUS_XR.config, board.json, README.md
  boards/speedybee-f405-wing/  defaults.cli, board.json, README.md
  models/rs7/               aircraft.json, qemu-profile.txt, visual/rs7.tscn, README.md
  models/trainer/           aircraft.json, inav.cli, env/{launch,ground-takeoff,acc-calibration}.json, visual/trainer.tscn, README.md
  presets/                  inav-trainer-launch.json, rs7-first-flight.json, rs7-fpv-osd.json, README.md
  firmware/                 FIRMWARE.md, nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf, speedybee-f405-wing/inav_9.1.0_SPEEDYBEEF405WING.hex
  core/                     qemu-system-arm.exe + DLLs (from -CoreDir)
```
The visuals' C# classes are compiled into the dll; the
`.tscn` is a text scene loaded by absolute path (`ResourceLoader.Load` works on a plain file outside `res://` in
the exported build — it has no `res://` dependencies; a visual scene that gains `ext_resource`s would break).
SITL tools are not shipped. **The core**: `-CoreDir` (default `$env:CORE_DIR`, else
`<repo>/dist/core`, where `core/qemu/tools/package-win.sh` puts it; `qemu-system-arm.exe` + its DLLs, self-contained) is copied to
`<dist>/data/core/` when it has `qemu-system-arm.exe`, and the script lists what it bundled (17 files, ~36 MB on
2026-09-28); without it the build has no core (Settings → Paths → core, or a remote core).
Checked 2026-09-28: `dist/FlightBench` copied to `%TEMP%` (no repo above), empty `APPDATA`, an RS7 bench in
`FB_BENCH_DIR`, `FB_RC_OFF=1 FB_SCREENSHOT=... FB_SCREENSHOT_ALT=1.0`: the app found `data/core`, read the manifest
from the core, checked and started the bench (31250), connected; `core/qemu/scripts/flight.py --plan liftoff`
flew it on the pilot socket; screenshot at 1.01 m, t = 36.15 s (`app/evidence/shots-2026-10-24/rs7-liftoff.png`),
then the app quit and stopped the core (no `qemu-system-arm.exe` left).

Checked 2026-09-29 (presets): the export printed `presets: bundled 3 files, 9.5 KB`, `firmware: bundled 2 files,
854.3 KB`, both presets' paths present, `core: bundled 17 files, 35.7 MB`, `data: 30 files, 36.5 MB`, `build: 217
files, 218 MB`. `dist/FlightBench` copied to `%TEMP%\fb-preset-test\` (no repo above), empty `APPDATA`,
`FB_PRESET=rs7-first-flight FB_RC_OFF=1 FB_SCREENSHOT=... FB_SCREENSHOT_ALT=1.0`: the main menu opened the preset
(copied to `benches/RS7___First_flight.json`), the flight screen read the manifest from the bundled core, checked
the session (paths relative, `root=` = `data/`: firmware / profile resolved under it; shift 2, realtime on),
started it on 31250 and connected; `core/qemu/scripts/flight.py --plan liftoff` flew it on the pilot socket: the
firmware reports `rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)`, armed, 1799 rpm, real-time 1.01x, screenshot at
1.01 m, t = 36.15 s (`app/evidence/shots-2026-10-24/rs7-liftoff.png`), then the app quit and stopped the core (no
`qemu-system-arm.exe` left). First-run main menu: `app/evidence/shots-2026-10-24/preset-menu-first-run.png`.

**Evidence: the plane** (2026-09-29, branch `app-plane`). The export printed `presets: bundled 4 files`, `firmware:
bundled 3 files`, the three presets' paths (visual included) present, `core: bundled 17 files, 35.8 MB`, `data: 42
files, 38.3 MB` (+ `boards/speedybee-f405-wing/defaults.cli`, `models/trainer/inav.cli`, `models/trainer/env/*.json`,
`models/trainer/visual/trainer.tscn`). `dist/FlightBench` copied to `%TEMP%\fb-plane-test\` (no repo above), empty
`APPDATA`, an empty `XDG_CACHE_HOME` (the core's `cfg-cache`: a true first start), `FB_PRESET=inav-trainer-launch
FB_RC_OFF=1 FB_SCREENSHOT_CONFIG=... FB_SCREENSHOT=<shots>/plane-launch.png FB_SCREENSHOT_ALT=10`:
the main menu copied the preset, the app read the manifest from the bundled core, checked the session (`machine
speedybee-f405-wing, icount shift=2, realtime on, control 31250, msp 31235`) and started `-M speedybee-f405-wing`,
connected; Status and Log showed the first start: `config: applying the board's CLI script (13 lines)` at t = 7.94
s, calibrating poses 1/6 … 6/6 (14.79 … 34.79 s), `config: applied, ACC calibrated` at 40.96 s — about 4 host
minutes, no timeout, no error (`app/evidence/shots-2026-10-24/plane-config-progress.png`, pose 3/6). Then
`core/qemu/scripts/flight.py --plan plane-launch --t0 55` (from the repo, on the app-started core's pilot socket):
armed at 56.09 s, `launch: ramp… 50 km/h at 27.8 m/s², pitch 10°` at 59.00 s, `launch: released at 13.9 m/s,
0.60 m` at 59.50 s; screenshot at 10.07 m, t = 62.66 s (`app/evidence/shots-2026-10-24/plane-launch.png`: armed, mode
`ANGLE+NAV LAUNCH`, rpm 9987, airspeed 14.1 m/s, thrust 10.9 N, elevator +2.7°, pitch +16° in the Model window,
the chase camera behind the climbing trainer), then the app quit and stopped the core (no `qemu-system-arm.exe`
left; flight.py then lost its socket, as expected). With `FB_RC_OFF=1` the switch row shows the app's own channels
(all off), not the pilot socket's.

RepoRoot default: in the editor = the parent of `app/`; in an exported build = `<exe dir>/data` when it has both
`boards/` and `models/`, else the nearest folder above the exe that has both (`<repo>/dist/FlightBench/` without
`data/` finds the repo), else the exe's folder → set it in **Settings → Paths**. The resolved root is logged at
start (`[app] repo root …`) and shown in the footer. A RepoRoot saved in `settings.json` wins (the exported app
shares `%APPDATA%\Godot\app_userdata\FlightBench\` — settings, benches, manifest cache — with the editor runs, so
an old saved repo path overrides `data/`; clear it in Settings → Paths).
Checked 2026-09-26: `dist/FlightBench` copied to `%TEMP%\fb-export-test\` (no repo above), empty `APPDATA`,
`fake_core.py`: repo root = `<exe>/data`, RS7 visual from `data/models/rs7/visual/rs7.tscn`, 266 states headless;
windowed flight screen + wizard board list (`nexus-xr  RadioMaster Nexus-XR`) render.

## Files
| | |
|---|---|
| `tools/export.ps1` | Windows export + `data/` (boards/models the app reads) next to the exe, see Export |
| `scripts/CoreClient.cs` | TCP + newline JSON client: one task, socket, token and bounded outbox per connection, lines tagged with the connection id (stale ones dropped), state owned by the main thread, events via `Poll()` (JsonElements valid during the call only); `Send` queues only while connected, otherwise drops (logged once) and returns false |
| `scripts/CoreLauncher.cs` | the local core: `CoreLocator` (exe, data root, local vs remote), `CoreCheck` (session-check report, `launch:` line), `CoreProcess` (started core: output lines, stop, job object), `CoreLaunch` (check → port probe → spawn → control port, on a task), port helpers |
| `scripts/Flight.Core.cs` | the flight screen's core: start / reuse / restart / stop, launch outcomes → dialogs (rejected, port in use, failed), crash, the Bench menu (its Core group: Core line, Restart core, Bench check, Core address…, Use local core, Close bench); `CoreManifest` (manifest from the core when none is cached) |
| `scripts/CoreLaunchTests.cs` | self-tests `corelaunch`, `corelaunch_reject`, `corelaunch_portbusy` |
| `scripts/CoreSession.cs` | the flight screen's link: this run's address (bench or `FB_CORE`), connect / reconnect, command policy, `subscribe` + fresh `rc` on every connection, `subscribe {"modes":true}` after every `hello` |
| `scripts/DevEnv.cs` | every `FB_*` knob, read once; one on/off rule; `SaveBench` / `SaveSettings` gates |
| `scripts/Json.cs` | shared serializer options, safe getters on dotted paths (`TryNum(el, "fc.cpu_pct")`, `TryArr3`), write-then-rename saving, bad-file backup |
| `scripts/RcMath.cs` | µs ↔ normalised, `Map` with the two-segment calibration (a calibration that is not min < centre < max = identity), deadzone, reverse, clamp |
| `scripts/SelfTests.cs` | `FB_SELFTEST`: RcMath, settings migration, bench round trip, signals restart, outbox policy, and the runner that also drives the wizard and flight-screen tests |
| `scripts/MainMenu.cs` | main menu: recent benches (name · board/aircraft · last used; double-click = start), Presets (name · description; double-click / Open preset; first on a first run with "Start with a preset"), Load / Edit / New / Settings / Exit, `FB_BENCH`, `FB_PRESET` |
| `scripts/Presets.cs` | bundled presets: `<data root>/presets/*.json` listed as benches, their paths resolved under the data root, `Open` = copy into the benches folder (or the existing bench of that name) |
| `scripts/Ui.cs` | shared look of the non-flight screens: blueprint grid background, centered max-width card, title, footer (repo · version), theme overrides, UI scale (`ApplyScale`), check-box icons |
| `scripts/DevHooks.cs` | autoload: applies the UI scale, logs the repo root; `FB_SCREENSHOT`, `FB_SCREEN`, `FB_WIZARD_STEP`, `FB_FAKE_JOY`; strips joypad events from `ui_*` actions |
| `scripts/Wizard.cs` | new / edit bench wizard (`Wizard.Open`; edit = a copy of the bench, its own name saves over it): board + firmware file → aircraft → wiring graph + inspector (the device's manifest props) → `diff all` text (Load profile: `models/<aircraft>/**/<aircraft>-profile.txt`) → summary; a name whose bench file exists asks: save as `name-2` / overwrite / back |
| `scripts/Manifest.cs` | the core's manifest (boards with pads — label, kind, flags, pins, onboard device — and the older slots as fallback; devices with ports/props/faults/signals/implemented; aircraft/actuators): model, parser, source chain (live → `FB_MANIFEST` → `user://manifest.json` → built-in fallback) |
| `scripts/WiringGraph.cs` | wiring graph on `GraphEdit` over `BenchData.Wiring`: port kinds and the UART direction rule (`WiringCatalog`), repo fallbacks + autofill (`BoardInfo`: manifest `defaults`, else `boards/<b>/*.config` by the RS7 rule, + onboard devices), device nodes (one output per device port) left, two board nodes right (`· ports`, `· on board`; one input per pad), onboard devices grey with a badge, detach / put back, `assumed` badge on migrated entries; editable in the wizard, read-only in the flight Wiring window |
| `scripts/FaultsPanel.cs` | Faults window: rows per manifest fault grouped by device, toggle + params → `fault`, lit from `state.faults`, keybind cell; rows of devices not present on the bench greyed "not wired" |
| `scripts/Keybinds.cs` | bench functions (fixed, window toggles, one per fault) and their binds (`AppSettings.Keys`); key/joypad event → function |
| `scripts/Settings.cs` | global settings UI (interface/UI scale, paths, transmitter receiver-tab: device, calibrate, presets, per-channel source/detect/reverse/min-centre-max + live bars, raw axes; keys: function / bind / Bind / Clear), `StickView` |
| `scripts/AppSettings.cs` | settings model, `user://settings.json` (saved on leaving Settings, also when the window closes there; a file that does not parse is copied to `settings.json.bad-<time>` first); `Repo` (boards/, models/ folders) |
| `scripts/Bench.cs` | bench model (board, firmware, aircraft, wiring = device port → pad entries + detached onboard devices + wiring-graph node positions, board config text + optional board config / profile files, optional description, window layout), `user://benches/*.json`, old `slot` benches migrated on load as the core reads an old-form session (assumed ESC/servos added); `ToSession` = the core's session file in the new wiring form (Firmware window → **Save session file…**) |
| `scripts/Flight.cs` | flight screen: world, top bar (left: Menu, Bench — Edit / Save as / Save / the Core group (Core line, Restart core, Bench check, Core address…, Use local core, Close bench) / Functions / Config —, Windows, View, Settings, Copy configurator address; centre: the bench title, not clickable; right: Disconnect), embedded windows' content, time bar, key dispatch (`Run(function id)`), state → aircraft, RC sending; core commands only while connected (a refused one leaves the local toggle / pause / CLI text as it was); the Battery toggle follows `fc.power`; log window keeps the last 2000 lines (Ctrl+L in the CLI line clears it), stdout gets the app's own lines (not the core's log stream) |
| `scripts/CliHistory.cs` | the Log / CLI command history: global `user://cli_history.txt` (`FB_CLI_HISTORY`), append + flush per command, consecutive duplicates dropped, cap 1000 with compaction on load, prefix recall |
| `scripts/CliLine.cs` | the CLI input line: Enter sends (and stores), Up / Down / Escape browse the history, Ctrl+L clears the log; keys taken in `_GuiInput` before LineEdit so the caret stays |
| `scripts/FlightWindows.cs` | the flight windows: made from the layout snapshot taken on entry, `FB_WINDOWS`, fitted into the visible area without changing the saved layout (wanted vs fitted rect), written back into the bench; `InputRelay` for focused windows |
| `scripts/Flight.Plane.cs` | the plane on the flight screen: Launch button / key / overrides popover, every core `event` formatted and logged (config line timing, crash opens Aircraft), the Transmitter window's hint line (`Keybinds.Hint`) and `switchOrder` line; `AircraftInfo` (FDM kind from the manifest, else `models/<id>/aircraft.json`) |
| `scripts/PlaneStatus.cs` | `PlaneStatus` (`state.plane` + the plane's events → the Aircraft rows and log lines), `ConfigProgress` (the `config` events + function steps / calibration poses → the Status config line and its life, the firmware-wait text, replay), `CrashInfo` |
| `scripts/StatusPanel.cs` | the Status window: `FcStatus` (state / hello → link, firmware with mode tags, sensors) + `StatusPanel` (the four lines) |
| `scripts/AircraftPanel.cs` | the Aircraft window: `AircraftReadout` (state → rows; plane / heli) + `AircraftPanel` (rows, crash banner, Reset aircraft) |
| `scripts/WindowsTests.cs` | self-tests `status_compact`, `aircraft_window`, `transmitter_owns_switches`, `core_menu`, `layout_migration`, `core_fixtures` (models, the modes screen, the real core's captures) |
| `scripts/PlaneTests.cs` | self-tests `plane_state`, `plane_visual`, `launch_cmd`, `config_progress` (model checks + the flight screen with injected messages) |
| `scripts/Modes.cs` | the firmware views' model: `ModesSnapshot` (`modes`: boxes + aux ranges; the bench's `modes` snapshot), the switch rule (`DerivedSwitch` / `SwitchPosition`: positions, labels, µs sent), variant, AND labels, active boxes from `state.fc.boxes` / `fc.mode`, `OsdLine` (the Status sensors line's OSD), `FirmwareDiff` (`diff`; the bench's `firmware_diff`) |
| `scripts/TxModel.cs` | the bench's transmitter model (`transmitter`: per channel 5..16 `auto` / `switch2` / `switch3` / `switch6`, label, start), the migration of old benches, the RotorFlight default (`firmware_family` from `boards/<board>/board.json`), the Transmitter rows (static + derived, channel order) |
| `scripts/TxModelTests.cs` | self-test `tx_model` (pure) |
| `scripts/Flight.Modes.cs` | the flight screen's firmware views: the Transmitter rows from the bench's transmitter model + `modes` (click / switch keys → `RcInput.Override`), Update modes, `modes` errors, the Functions and Config windows (was Firmware config windows (Show cached / Refresh from firmware), Settings over the bench |
| `scripts/EnvPanels.cs` | `FunctionsPanel` (Functions window: catalog buttons → `function`, progress lines, "at first start") + `FunctionProgress`; `DiffView` (the right half of the Config window: the firmware's `diff all`, header with source / time / stale, Show cached, Refresh from firmware (reboots) with a confirmation, only disarmed, Copy) |
| `scripts/BenchState.cs` | the bench's config state file: `<cfg-cache>/<bench-id>.state.bin` (bench-id = the bench's file name; `<bench dir>/state/` under `FB_BENCH_DIR`), its files for Delete |
| `scripts/Flight.Bench.cs` | the flight screen's bench-wide actions: Edit bench in place (the wizard over the screen, Save & start → restart with the same bench state), Reset firmware config to first start, Delete bench, the CLI-mode indicator and exit CLI |
| `scripts/Flight.BenchDev.cs` | `FB_CLI_SEND`, `FB_DIALOG`, `FB_CONFIG`, `FB_EDIT_RESTART` |
| `scripts/WizardScript.cs` | the wizard summary's **Firmware config at first start — by layer**: a session-check of the bench as it is on the pages → `LayersView` (Check again, Copy) |
| `scripts/ConfigLayers.cs` | `LayeredConfig` (a session-check report → lines with layer / source / disabled + conflicts: the core's `config_json` / `config: [layer:source]` / annotated `config_cli|` / `config_conflict`, else inferred by the app from the bench's files; `SettingKey`, computed conflicts), `ConfigChanges` (`diff against script`), `BenchFiles` (the bench's own files next to it: bench lines `./<bench>.cli`, copy-on-write of a preset's aircraft / board file into `./<bench>.files/`, rename, delete, export as a preset folder, import) |
| `scripts/LayersView.cs` | the layered view (wizard summary, Config window): a tree per layer + a conflicts group, a line's actions (edit its file at the line / disable / override in the bench lines), the inline file editor (in place, or the bench's copy of a preset's file), the bench lines box |
| `scripts/ConfigWindow.cs` | `ConfigPanel`, the **Config** window: Script ↔ Firmware (LayersView + DiffView), Changes (diff against script → bench lines / disable), the operations with their confirmations |
| `scripts/Flight.Config.cs` | the flight screen's side of the Config window: layers from the launch's session-check / a local check / the core's export, Apply script now, Snapshot → bench lines, Export as preset, Changes, `config` answers; the boot hint next to the configurator address |
| `scripts/LayersTests.cs`, `LayersTests.Screen.cs` | self-tests `layers_view`, `bench_lines`, `config_changes`, `snapshot_export`, `boot_hint` (fixtures, a python fake-core session-check, a spawned fake core, the modes screen) |
| `scripts/BenchTests.cs` | self-tests `bench_delete`, `wizard_script_view`, `cli_indicator`, `edit_in_place` (models + the modes screen, the wizard over the root) |
| `scripts/ModesTests.cs` | self-tests `modes_switches`, `functions_panel`, `topbar`, `osd_line`, `diff_view` (fixtures + the flight screen against the fake core); the modes screen also runs the windows' screen checks (WindowsTests.cs) |
| `scripts/ConfiguratorAddress.cs` | the Copy configurator address form: `host:port` for the iNav Configurator, `tcp://host:port` for RotorFlight / Betaflight and whenever unknown; the firmware, best source first: the `modes` `variant` (INAV / RTFL / BTFL), the core's firmware name (`hello.firmware`, or its log line `firmware says: …` when the firmware answered after the client connected), else the bench's firmware file name (iNav = the file name, not its folder, contains `inav` and not `rotorflight` / `betaflight`) |
| `scripts/Flight.Dev.cs` | `FB_KEYS_SELFTEST`, `FB_FAULT_SCRIPT` |
| `scripts/OsdPanel.cs` | `OsdBuffer` (rows×cols chars + attrs + the firmware's font, applies `osd` messages) and `OsdPanel` (draws it on a fixed cell grid, scaled to fit) |
| `scripts/OsdFont.cs` | MAX7456 `.mcm` decoder → cached glyph atlas (256 or 512 glyphs) for `OsdPanel`, the font per `osd.font`, the glyph index per firmware; `RenderRow` / `RenderBuffer` for PNGs |
| `assets/osd/` | `default.mcm` (RotorFlight's font), `inav-default.mcm` (iNav's), `FONTS.md` (their provenance: source, tag, sha256), `NOTICE` (GPL-3.0) |
| `tests/osd-inav-boot-canvas.txt` | the core's captured iNav canvas (core branch `inav-osd`, `core/qemu/evidence/inav-osd/`, commit 8df8a81): `osd` / `osd_font` self-tests (not in an exported build: those checks are skipped there) |
| `scripts/Signals.cs` | Signals window: `SignalStore` (state flattened + core `signals`/`samples`), `PlotView` (shared time axis, per-trace or shared Y scale, min/max per pixel), `SignalsPanel` (filter, checkboxes, values, CSV) |
| `scripts/Aircraft.cs` | aircraft node: loads `models/<name>/visual/<name>.tscn` (placeholder boxes if missing), NED pose → Godot (x = E, y = −D, z = −N) + `cg_height`, body axes FRD (toggle), attitude-only mode for the Model window |
| `scripts/AircraftVisual.cs` | `IAircraftVisual` (Build / ApplyState / Tick) + `VisualKit` (primitive meshes, rods between points, labels) |
| `../models/<name>/visual/` | the aircraft's visual scene + its C# class, compiled into the app (`<Compile Include="../models/*/visual/*.cs" />` in `FlightBench.csproj`) |
| `scripts/RcInput.cs` | transmitter → 16 channels (µs): per-channel source (none / ui / axis / button / 3-pos buttons), calibration, reverse, hot-plug; the keyboard layer over channels 1..4 and the ownership rule (`Owner`: the last input that changed a channel holds it); `IJoyReader` (Godot Input or the dev fake) |
| `scripts/RcWidgets.cs` | channel names, `ChannelBar` (live 1000..2000 µs bar) |
| `shaders/grid.gdshader` | 1 m / 10 m blueprint grid with distance fade |
| `scenes/*.tscn` | one root node per screen with its script; everything else is built in code for now |

## Benches
A bench is one JSON file in `user://benches/<name>.json` (Windows: `%APPDATA%/Godot/app_userdata/FlightBench/benches/`;
`FB_BENCH_DIR` moves it for dev runs). **New bench** (main menu) runs the wizard from defaults. **Edit bench**
(main menu, selected bench; or flight screen → **Bench → Edit bench…**, which saves the layout first and opens the
wizard over the running bench: see **Bench state, edit in place, delete**) runs the same wizard on a copy of it, titled "Edit bench: <name>": saving under the same name
overwrites that bench without asking, another name makes a new bench and leaves the original as it was (a
name some other bench already has asks: save as `name-2` / overwrite / back); Cancel changes nothing, Save &
start goes to the flight screen with the result (over the flight screen: applies it there, the core restarted with the
same bench state). **Delete…** (main menu) / Bench → **Delete bench…** removes a bench with its session and bench-state files. Flight screen → **Bench → Save** writes the bench now (what
leaving the screen does: window layout, camera, body axes, core address, firmware path and watch);
**Save as…** asks a name and writes a copy with all of that, the screen goes on with the copy (the original
file keeps what it had on disk). Runs that may not write the bench (headless, layout knobs, self-tests) only
log that they did not.

**Checked signals** (`BenchData.Signals`): the Signals window's checked set is saved with the layout (same gating) and re-checked when the flight screen opens (core ids subscribed once the first state tells them apart); `FB_SIGNALS` replaces it for that run.

**Radio unplugged** (Settings → Transmitter, "When the radio (joypad) disconnects, fail the receiver link", default on): on the flight screen the named radio going away sends `fault crsf link_lost on` (if crsf is wired and the manifest has the fault; otherwise the log says why), the channels hold their last values (the keys are not read either, the firmware goes to failsafe); when it is back, the first read channels go out and then `on:false`. A radio absent from the start is not a loss.

**Transmitter model** (`BenchData.Transmitter`, JSON `transmitter`; `TxModel.cs`; decisions 2026-10-23): what each channel
5..16 *is* on this aircraft's radio — one object per channel keyed by its 1-based number, absent channel = `auto`:
```json
"transmitter": { "5": { "type": "auto" }, "6": { "type": "switch2", "label": "throttle" } }
```
`type` = `auto` (a switch derived from the firmware's modes; no row until the firmware gives ranges on the channel),
`switch2` (1000 / 2000), `switch3` (1000 / 1500 / 2000) or `switch6` (ArduPilot's flight-mode channel, 1165 / 1295 /
1425 / 1555 / 1685 / 1815); `label` (optional, shown as the row's name: `ch6 throttle`); `start` (a static switch's
start position, 0 = lowest, left out when 0). Edited in the wizard's Summary step → **Transmitter** (channels 5..12,
**More channels (13–16)**; type, label, start position for static types); Save & start over the flight screen rebuilds
its rows. A **new bench** gets its board's default (`boards/<board>/board.json` → `firmware_family`): RotorFlight →
ch5 `auto`, ch6 `switch2` `throttle`; other families: all `auto` (no block); an edited bench keeps its model.
**Migration**: a bench without the block (saved before 2026-10-23) is shown and driven with its board's family default
(`TxModel.Effective`: a RotorFlight bench gets ch6 `switch2` `throttle`, so a bench copied from the RS7 preset before the
change takes off like a fresh one) without the file being rewritten; the block is written only when the model is edited
(Edit bench → Transmitter starts from the effective model). A bench with an old `switches` list gets the default plus a
name of that list as the channel's `label` where the default has no static switch there and the bench's `modes` snapshot
has no range (`TxModel.Migrate`; no static switch is made from old switch data: the old layout was the firmware's
modes). An explicit block, even an empty `{}`, is kept as written and gets no default on top. Channel **sources** are not part of the bench: they are
the user's (`settings.json` → `transmitter.source`: `none | ui | axis n | button n | buttons n,m`, Settings →
Transmitter); the keyboard is not a source but a layer over channels 1..4 (Settings → Keys → Sticks), and an old
`keyboard` source reads as `none`.

**Core time** (wizard, summary step; `BenchData.Time`, the session's `time` block): *Virtual CPU speed* =
`icount_shift` 1..3 (1 = 500 MIPS, twice a real F722: slowest emulation, lowest firmware load; **2 = 250 MIPS ≈
a real F722, the default**; 3 = 125 MIPS: fastest emulation, the firmware sees double load); *idle skip*
(default on: the core fast-forwards the firmware's idle loop, same flight ~2x faster; off = every instruction
executed); *Real-time* (`time.realtime`, **default on** since 2026-09-29: never faster than wall clock, for
flying by hand; a bench with an explicit `realtime` keeps it, nothing is migrated; turn it off for runs that
should go as fast as they can). It takes effect when the core starts from the session (the app starting the core, or Firmware window →
Save session file… for `run.sh`), so it is edited in the wizard only, not from the flight screen (the bottom bar's *real-time* toggle is the
running core's, for this run: it starts as the bench says when the app started the core, off otherwise). Benches
saved before the `time` block (or a `time` block without `realtime`) get the defaults, real-time on included.

## Presets
Ready benches bundled with the app, so a fresh install can fly at once: `presets/*.json` at the repo root
(`<exe dir>/data/presets/` in an exported build), read from `<data root>/presets` (`Presets.Dir`; the data root =
`CoreLocator.DataRoot`). Each is a bench in the wizard's form plus an optional `description`; see
`presets/README.md` for what they hold and how to add one.
- **RS7 · First flight** (`rs7-first-flight.json`): Nexus-XR, firmware `firmware/nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf`,
  board config file `boards/nexus-xr/NEXUS_XR.config`, profile file `models/rs7/qemu-profile.txt`, aircraft rs7, the
  RS7 wiring of `core/qemu/sessions/rs7.json` (gyro, baro, receiver on uart5, ESC on motor1 bidirectional, servos 1–4 →
  swash_a/b/c, tail; flash onboard), `core: local`, time shift 2 + idle skip + real-time, Status + Transmitter +
  Log / CLI open.
- **Trainer · FPV with OSD** (`inav-trainer-fpv-osd.json`): the trainer preset (GPS module, compass, battery, SPL06 kept)
  + `msp_osd` on UART4 (`serial 3`, the F405 WING's free UART next to the GPS), the OSD window, FPV camera, OSD overlay
  on. `dist-core` (2026-09-30, `main` b6b1d5c) models `msp_osd` on this board (`feature OSD`, `serial 3 33554432 …`,
  `osd_video_system = HDZERO`): the OSD renders iNav's own font live once the firmware answers
  (`app/evidence/shots-2026-10-24/osd-inav-live-trainer.png`).
- **RS7 · FPV with OSD** (`rs7-fpv-osd.json`): the same + `msp_osd` on PORT-A (uart4) as `rs7-osd.json`, the OSD
  window open, FPV camera, OSD overlay on.

Main menu → **Presets** (double-click / **Open preset**, or `FB_PRESET`): `Presets.Open` copies the preset into the
benches folder under its name (`BenchData.PathFor(name)`, e.g. `RS7___First_flight.json`; `FB_BENCH_DIR` moves it) and
starts it like any bench (local core). The bundled file is only read. A bench of that name that exists is opened as it
is (the user's changes kept) and the flight screen's log says "a bench of that name exists … opened it as it is";
**Delete…** it (main menu, or Bench → Delete bench…) or rename it to get the preset fresh. No benches yet: the Presets list is on top with "Start with a preset".

**Paths relative to the data root.** A bench's own paths — `firmwarePath`, `boardConfigFile` (session `board_config`;
empty = the board's `boards/<board>/*.config`, as before) and `profileFile` (session `profile`, applied after the board
config and before the pasted `boardConfig` text) — may be absolute, `~/...` or relative. `ToSession` writes them as
they are (`\` → `/`): a relative path stays relative in the session file and the core resolves it against the
`root=` the app passes (the data root), never against the app's working directory. The wizard shows each resolved
the same way (`BenchData.ResolvePath`): "→ <path> · found", or "· MISSING (relative to the data root …)"; Browse…
stores a file under the data root as a relative path (`BenchData.RelativeTo`), anything else absolute. A relative
firmware path in a bench flown against a remote core is resolved by that core against its own root.

## Bench state, edit in place, delete
Product owner's test 2026-10-01: a bench made from a preset could not be deleted; Edit bench did not show the starting
CLI script; every re-entry lost the calibrations and the configurator's modes (core side: branch `bench-state`, being
built on the homelab); after the reboots it was not visible whether the firmware sat in the CLI; "leave the bench → Edit
bench → start → everything again" was painful. The app side (branch `app-bench`), built against the core contract
(`bench-state=` in `-M`; `config {state:"restored", source:"bench"|"snapshot"}`, `config {state:"applying", delta:true,
lines:N}`, `config {removed:[…]}`; `{"type":"config","reset":"first_start"}`; `state.fc.cli` + event `cli {open, by}`;
`{"type":"cli","exit":true}`) and `tools/fake_core.py`, which speaks it:
- **Bench state** (`BenchState.cs`): every start of a bench passes the same `bench-state=<cfg-cache>/<bench-id>.state.bin`
  (the core's default cfg-cache: `XDG_CACHE_HOME`, else `%LOCALAPPDATA%` / `~/.cache`, + `flightbench/config`;
  `<bench dir>/state/` when `FB_BENCH_DIR` moves the benches). bench-id = the bench's file name (`BenchData.SafeName`: letters, digits, `-`, `_`; any other run of characters → one `_`, so "RS7 — first flight" → `RS7_first_flight`; a bench's own files are `./<id>.cli`, `./<id>.files/`, and `BenchData.OwnRel` refuses a `./` path that leaves the folder — `./../x` in a preset is ignored, not copied): a bench saved under
  another name is another bench with its own first start. Only when the core lists the option; else logged.
- **Edit bench in place**: Bench → Edit bench… opens the wizard in a window **over** the flight screen (keys go to it;
  the core keeps running). **Save & start** writes the bench, the screen takes it (wiring / faults / functions / title
  / firmware path follow) and restarts the core: stop → session-check → start with the same bench state → connect; the
  log and the windows stay. Status then shows `config: restored (bench state)` / `config: applying 4 new lines
  (delta)…` → `config: applied`; lines the bench no longer has come as `config {removed:[…]}` and are logged ("left in
  the firmware, not reverted"). A new board / aircraft / visual reopens the flight screen on the edited bench (the
  running core handed over and stopped first). Cancel changes nothing. A remote / FB_CORE bench reconnects.
- **Reset firmware config to first start…** (Bench menu, asks, while connected): `{"type":"config","reset":"first_start"}`.
- **Firmware config at first start** (wizard summary): now **by layer**, editable at the source — see **Config layers**.
- **CLI mode**: `state.fc.cli` and the `cli` event set it; Status's firmware line reads `firmware: in the CLI — MSP
  blocked` with **exit CLI (reboots)** next to it (not during the core's own first start: that line wins), the Log /
  CLI row the same label + button, the Copy-configurator-address tooltip says the configurator cannot connect. The
  button sends `{"type":"cli","exit":true}`.
- **Delete** (main menu **Delete…**; flight screen Bench → **Delete bench…**): a confirmation naming the bench file,
  its session file and its bench-state files (`BenchData.FilesOf`); the flight screen stops its core first (and a core
  kept running for that bench), deletes, and goes to the main menu without writing the bench again. A file inside the
  bundled presets is never deleted (`BenchData.IsBundled`).

Self-tests (`BenchTests.cs`; the screen parts run on the modes screen after its own tests, against the plain fake core):
`bench_delete` (a temp benches folder: a preset opened, its session + state files next to another bench's; Delete takes
exactly its four files, the other bench and the bundled presets byte-identical, the confirmation names every file, the
preset reopens fresh; screen: Bench → Delete bench → disconnected, files gone, not written again), `wizard_script_view`
(the CLI and blob report parsers; the wizard over the root on the trainer preset, summary step: the packaged core's
session-check → 134 CLI lines with the board / aircraft / GPS lines, read-only), `cli_indicator` (FcStatus rules; screen:
a CLI line → the fake core's `cli` event + `fc.cli` → Status line + button, Log label, tooltip; the button sends the exit
command → hidden again), `edit_in_place` (the bench-state path per bench, `-M …,bench-state=` with commas doubled, none
for an older core — probed on `FB_CORE_EXE`; screen: the wizard over the connected screen, Save & start → written,
applied in place, the same bench-state file, reconnected; the delta events in Status; Reset firmware config sent → the
fake core's first start).

What is verified where (2026-09-29): **against the packaged core** — the session-check script view (trainer 134 lines),
the in-place restart (`FB_EDIT_RESTART=1` on RS7 · First flight, ports 31295..31299: first core stopped, the bench
checked and started again on the same ports, the firmware answers on the new core), the option probe (`bench-state`
not listed: left out, logged), the trainer presets' session-check (GPS / compass / battery / SPL06 modelled; `msp_osd`
left out on the F405). **Only against the fake core / injected messages** — the `bench-state=` file actually used by
a core, the `restored (bench)` / `applying (delta)` / `removed` events, `config reset`, the `cli` event and `{"type":"cli",
"exit":true}` (the packaged core has `fc.cli` but not the event or the exit command). Evidence:
`app/evidence/shots-2026-10-24/bench-wizard-script.png`, `app/evidence/shots-2026-10-24/bench-cli-indicator.png` (fake core, `FB_CLI_SEND=status`),
`app/evidence/shots-2026-10-24/bench-delete-dialog.png`.

**Modes that touch or overlap** (the owner's "did not refresh"): the app's rule and the Transmitter rows are right —
1300–1700 + 1700–2100 of two boxes = three positions (1700 is the upper), of one box = one position, a partial overlap
= four, and every new `modes` message rebuilds the rows in place (no stale row, a channel without a switch released);
`modes_switches` covers it. Whether the core sends a new `modes` after a configurator save is the core's part.

## Config layers
Product owner 2026-10-02: "we build a developer tool — nothing implicit or read-only". The config the core types into the
firmware is shown as explicit **layers with provenance**, editable at the source, with explicit operations (reconciled
2026-09-30 against the real `config-layers` core, docs/protocol.md, authoritative; `tools/fake_core.py` speaks the same
contract for offline dev / self-tests).

**The layers** (in the order the core assembles them; one setting in two layers is a *conflict* and the **later layer
wins**): **1 board** (the board's base script, `boards/<board>/defaults.cli`, a blob board's `*.config`; the bench's
`boardConfigFile` when set) · **2 aircraft** (`aircraft.json` `firmware_cli`, e.g. `models/trainer/inav.cli`; the
aircraft's profile file on a blob board) · **3 profile** (the `profile` files / `profile_text`) · **4 device** (the wired
devices' lines, `device ublox on uart3`) · **5 bench** (the bench's own lines: `bench_cli`, a file next to the bench; the
pasted board config of step 4 counts as the bench's too) · (6 the firmware's current `diff all`, the right half of the
Config window: its source is the firmware).

**What the app reads** (`LayeredConfig.Parse`): the core's `config_json:` (one line, or a block until the JSON closes:
`{"kind","layers":[{layer,file,path}],"lines":[{text,layer,source,ref}],"disabled":[{text,layer,source,ref,by}],
"conflicts":[{setting,lines,wins}]}` — `ref` is `"<layer>:<n>"`, 1-based within its layer; `wins` names the winning
line's `ref`) — the authoritative, structured form; else its textual twin `config_src: [<layer>:<source>] <text>` /
`config_disabled: [<layer>:<source>] <text> (disabled_lines '<entry>')` / `config_conflict: <setting>: [<layer>:<source>]
<text> -> [<layer>:<source>] <text> (the last wins)` (a core / stand-in without `config_json`); else the raw
`config_cli| <text>` lines with no provenance at all (a core before config-layers) — those get **attributed by the app**
(`Infer`: the bench's board file, aircraft `firmware_cli`, profiles, pasted board config, the report's `config:` device
lines and the bench lines, matched line by line in the core's order; every such line is marked "(inferred)", a line
nobody has stays "not traced") and **conflicts computed by the app** (`SettingKey`: `set x`, `feature X`, `serial N`,
`aux N`, `smix`/`mmix`/`servo N`, `resource A B`, `map` — with the profile a script switched to, `control_profile 2`;
marked "found by the app"). The `config:` lines (after `machine:`) are unchanged throughout: the wired devices' own
lines, disabled ones included, never annotated — `DeviceLines`, not the layered view's source. Honest notes when a core
ignores what the bench asks: "this core does not know bench_cli / disabled_lines (a core before config-layers)" — a
disabled line is then *not* shown as skipped.

**The layered view** (`LayersView`: the wizard's summary step and the Config window's left half): a tree — a **conflicts**
group first (each setting, its lines, the winner starred, the overridden ones ⚠, amber), then one group per layer (large
ones collapsed) with per line the text, the source (`models/trainer/inav.cli:25`, `device ublox on uart3`, `./trainer.cli:1`;
files next to the benches as `./…`, the data root's relative) and an **on** check box. A selected line offers:
- **Edit <file> at line N** (board / aircraft lines, also a layer's group row — `defaults.cli` may have no line yet; double-click
  too): the file in an inline editor, the caret on the line. The editor's header says what **Save** does *before* saving:
  - a bench of the user's own: writes the file **in the data root** ("every bench with this aircraft uses it");
  - a bench made from a bundled preset (`preset` in the bench JSON, set by Presets.Open; or a bench named like a preset):
    the preset's files are read-only bundles — **copy-on-write**: the aircraft folder (`models/<aircraft>/` without
    `visual/`) is copied to **`<benches folder>/<bench>.files/models/<aircraft>/`** (the copy's `aircraft.json`
    `firmware_cli` points at the copied CLI, absolute; the bench's `aircraftFile` = `./<bench>.files/models/<aircraft>/aircraft.json`,
    the session's `aircraft` then points there), a board file to `./<bench>.files/boards/<board>/<file>` (the bench's
    `boardConfigFile`). The repo / install files stay as they are. (`<benches folder>` = `user://benches`, or `FB_BENCH_DIR`.)
- **Disable / Enable line** (and the **on** box): the bench's `disabledLines` (the exact line text; the session's
  `disabled_lines`, which also takes `layer:index` refs), then a new session-check.
- **Override in bench lines** (not on layer 5): the line copied into the bench lines box to change it there (typed last: it wins).
- **5 · bench lines**: an editable box = the bench's `bench_cli` file, **`<benches folder>/<bench>.cli`** (the bench JSON's
  `benchCli` = `./<bench>.cli`; the session gets the absolute path, only while the file exists). **Save bench lines** writes it
  and checks again; **Revert** reloads it. Unsaved text survives a new check.
- Every edit (file, bench lines, disabled line) runs the session-check again: the view is always the core's assembly of the
  bench as it is. A bench saved under a new name copies its own files (`<new>.cli`, `<new>.files/`, `firmware_cli`
  re-pointed; a bench new in the wizard *moves* them); Delete takes them (`BenchData.FilesOf`).
- The wizard's summary text: `Board config (step 4): none | 1 line | N lines` (was "1 lines" for an empty one), `Bench
  lines (layer 5): …`, disabled lines, the aircraft copy. **`FB_WIZARD_SCRIPT_EDIT` is gone** (no hidden modes).

**The Config window** (Bench → Config…, Windows → Config; the window's saved place and key bind move from "Firmware
config"): tabs **Script ↔ Firmware** (left the layered view — "the script this core started with" from the launch's
session-check; after an edit "the bench as it is now — not sent to the firmware yet"; a bench the app does not start: a
local session-check, or the core's `config export` — right the DiffView) and **Changes** (`{"type":"diff","against":"script"}` →
`{sent, current, changed:[{setting,script,firmware,layer,source}], only_firmware:[{setting,firmware}], only_script:
[{setting,script,layer,source}]}` — `script` / `firmware` are the FULL LINE TEXTS, typed as is: three groups — *changed by the
configurator* (`set nav_fw_bank_angle = 35 → set nav_fw_bank_angle = 40`), *only in the firmware — added*, *only in the
script — removed / at the firmware's default*; **Selected → bench lines** / **All changes → bench lines** put the
firmware's whole line into the bench lines box (`set nav_fw_bank_angle = 40`; then Save bench lines), **Disable the
script's line** for an only-script one. A core
before config-layers answers with a plain diff (it takes `against` for a refresh — the firmware reboots; checked on the
packaged core): the Changes view says so).
**Operations** (a row at the top; the ones that reboot ask first and are off unless connected and `fc.armed` is known false):
**Apply script now (reboots)…** (`{"type":"config","apply":true}`, **full** → `"full":true`; the session file is written again
first so a core that re-reads the bench takes the edits), **Re-run <function>…** (each function of the aircraft →
`function`), **Reset to first start…** (the existing `config reset`), **Snapshot firmware config → bench lines…**
(`{"type":"config","snapshot":"bench","path":<the bench's bench_cli, absolute>}`; the bench's `benchCli` set first; the core
takes `diff all` — reboots — and answers `config {snapshot:"written", path, lines}` (a `config` message or event): the
Config window and an open Edit-bench wizard reload layer 5), **Export as preset…** (a folder: `{"type":"config","export":true}`
when connected — the core's layered assembly + current diff, 3 s — then `<id>.json` (the bench as a preset: its own files as
`./<id>.cli` / `./<id>.files/…`, no lastUsed / wiringLayout / preset / modes / firmware_diff), `<id>.cli`, `<id>.files/` (only
when the bench has its own copies), `<id>.README.md` (files, the layers with their lines, conflicts, disabled lines, the
diff); install = copy the folder's files into `<data root>/presets/`; Presets.Open copies the `./` files next to the new
bench and re-points `firmware_cli`). Refusals / results show as one line under the operations.

**Boot hint**: after a `reboot` event, while `state.fc.msp` is false, the top bar shows next to **Copy configurator
address** "firmware booting — the configurator can connect in ~N s" (N = the last boot the app measured: `reboot` → `fc.msp`
true, host seconds, counting down; before the first measurement "… once it answers"); tooltip: *after "Save and Reboot" in
the configurator wait for the green dot*. **exit CLI** checked on the packaged core (RS7): `{"type":
"cli","exit":true}` → `cli {open:false}` + `reboot`, MSP back 5.6 s later; outside the CLI `error` "cli: the firmware is not in
its CLI". A pressed switch position with the label "—" was invisible (its orange box had narrower content margins than the
button's own, so the text was clipped away; "none" read "non"): fixed.

**The fake core** (`tools/fake_core.py`) speaks the real contract (docs/protocol.md, verified against the packaged core
2026-09-30 — see below): `config apply` (applying / applied / reboot, refused armed), `config snapshot` (writes the diff's
CLI lines to the path through a temp file + rename, answers written), `config export` (`{export:{bench,root,
assembly:<config_json>,script,applied,diff:{text,stale,source,at}}}`), `diff against script` (from its script — plain:
RS7-like lines; plane: today's `models/trainer/inav.cli` + the GPS lines — `changed`/`only_firmware`/`only_script` as full
line texts with layer/source), reboots with a ~2 s MSP gap, `--disarmed`, and `--session-check SESSION ROOT` (or the core's
own `-M …,session-check=on` form, so a `fake-check.cmd` wrapper — `@python <repo>/tools/fake_core.py %*` — can stand in for
`FB_CORE_EXE` in a screenshot run): `config:` (unchanged, device lines), `config_src`/`config_disabled`/`config_conflict`,
`config_cli|`, `config_json` (`layers`/`lines` with `ref`/a top-level `disabled` array/`conflicts` by `wins`), honouring
`bench_cli` and `disabled_lines`.

**What is verified where** (reconciled 2026-09-30 against `dist-core` built from `main` b6b1d5c, docs/protocol.md): **the
packaged core reports full layered provenance** — no inference needed any more. `wizard_script_view` (self-test,
`FB_SELFTEST=all`): the trainer preset's session-check → 154 CLI lines, "layers from the core" (board/aircraft/profile/
device/bench counts), not inferred. Live-check (this session, the "Trainer · FPV with OSD" preset, its own `bench_cli` +
a `disabled_lines` entry, `dist-core\qemu-system-arm.exe`, ports 31295-31299): the layered view shows `config_src`
provenance for every line (no "(inferred)"), the disabled `set gps_ublox_nav_hz = 10` device line reaches the core (shown
✕, unchecked), the bench line `set nav_fw_bank_angle = 42` reaches it too (layer 5, winning a conflict with the aircraft's
`= 35`) — `app/evidence/shots-2026-10-24/config-layers-live-trainer-osd.png`. `config_changes` / `snapshot_export` /
`edit_in_place` self-tests exercise apply / snapshot / diff-against-script against `dist-core` too (`corelaunch*`,
`presets`). **Only against the fake core / injected messages** (still, by design — these need a scripted firmware, not a
real one) — the exact `applying`/`applied`/`removed`/`reasserted` event sequence, the `cli` event and `{"type":"cli",
"exit":true}` on this board (the packaged core has `fc.cli` but not the event or the exit command), the boot hint's
countdown on screen.

Self-tests (`LayersTests.cs`; the screen parts on the modes screen before the bench tests): `layers_view` (fixture reports:
`config_json` (layers/ref/disabled array/conflicts by `wins`) + a block over several lines, `config:` (unchanged) beside
`config_src`/`config_disabled`/`config_conflict` without `config_json`; SettingKey with profiles; the trainer attributed
by the app with a computed conflict (a report without any provenance at all); the note for a core without
disabled_lines; the tree; the edit policy), `bench_lines` (a temp benches folder, the trainer preset as a bench,
`fake_core.py --session-check` as the core: bench lines → `./layers_bench.cli` → layer 5, last, winning; a device
line disabled → skipped; inav.cli edited → the aircraft copied next to the bench, the repo untouched, the check reads the
copy; the board group's defaults.cli → the bench's copy; rename copies; export → import re-points firmware_cli; delete),
`config_changes` (the fixture's three lists — full line texts —, BenchLineFor, the Changes tree, → bench lines; screen:
the fake core's answer), `snapshot_export` (a spawned `fake_core.py --disarmed` on its own port, stopped by PID: snapshot
file + answer, export (the `assembly`/`diff` shape), apply events; screen: Snapshot sends the bench's path, a written
answer reloads layer 5 in the Config window and the wizard, Export as preset → the folder with the core's layers in the
README), `boot_hint` (screen: exit CLI → reboot → "once it answers", measured, "in ~N s", tooltip, top-bar place). They
need `python` on PATH.
Evidence: `app/evidence/shots-2026-10-24/config-layers-wizard.png` (fake-core session-check: conflicts, a disabled device line,
bench lines, a selected conflict line), `app/evidence/shots-2026-10-24/config-layers-live-trainer-osd.png` (the packaged core, real
`config_src` provenance, a disabled device line and a winning bench line reaching it, "Trainer · FPV with OSD"),
`app/evidence/shots-2026-10-24/osd-inav-live-trainer.png` (the same bench, live, iNav's OSD font), `app/evidence/shots-2026-10-24/config-window-script.png` and
`app/evidence/shots-2026-10-24/config-window-changes.png` (`fake_core.py --plane`), `app/evidence/shots-2026-10-24/modes-switch-dash.png` (the pressed "—").

## Aircraft visual
`models/<name>/visual/<name>.tscn` is loaded at runtime by absolute path from the repo root
(`AppSettings.RepoRoot`); if the file is missing the flight screen shows the placeholder boxes. The
scene is a tree of named `Node3D` pivots — the actuator names from the model README (`swash_a`,
`swash_b`, `swash_c`, `tail`, `motor`) plus `rotor`, `swashplate`, `tail_rotor`, `tail_servo`, `fpv` —
in Godot body axes (x right, y up, nose towards −z), origin at the CG. Root metadata: `visual_class`
(the C# type implementing `IAircraftVisual`) and `cg_height` (skids → origin; the FDM puts the skids
at d = 0). The class is a plain C# object, not a Godot script, so the scene can live outside `res://`;
it builds the meshes under the pivots and animates them from each `state`.

RS7 (`models/rs7/visual/Rs7Visual.cs`), 700 class, layout after the RS7 Ultra manual: 1.58 m rotor,
1.1 m boom, open frame (side plates as rod outlines, no canopy), motor ahead of the main shaft with a
pinion on the main gear, ESC on an angled plate at the front, battery under the frame between the skids.
Swash servos (120° CCPM, names after RotorFlight's SWASH_TYPE_120: servo 1 = `swash_a` is the single
pitch servo) stand upright: `swash_a` on the centre line behind the main shaft (plate ball at 180°),
`swash_b` left-front (−60°) and `swash_c` right-front (+60°) as mirror images; each horn tip sits under
its plate ball (`metadata/horn` / `metadata/case` on the nodes), so the pushrods are vertical at neutral;
the 20 mm horn rotates by
θᵢ = `actuators.swash_i` × 30° (±10 mm of ball travel), its ball describes an arc, and the pushrod is a
rigid link from the horn ball to the plate ball: ball height = tip height + √(L² − horizontal offset²).
Plate = plane through the three balls: height = mean(hᵢ), tilt from the differences (slopes =
2/(3r) · Σ hᵢ·uᵢ). Blade pitch follows the plate height under each pitch link (±12°). Tail: servo on the
right side plate behind the main shaft under the boom, shaft across the frame, long horn standing up, swinging fore/aft (`actuators.tail` × 30°); the
pushrod runs under the boom to the bell crank at the tail case, whose second arm moves the pitch slider;
tail blades pitch ±20°. The blades
turn at the real head speed `rpm` (no blur disc), the tail rotor at 4.5 × head, the motor can at
`motor_rpm`. View → **Body axes + actuator labels** (X) shows FRD arrows and the actuator names.

## Plane
A bench whose aircraft has `fdm: "plane"` (the manifest's aircraft, else `models/<id>/aircraft.json`; today the
**trainer** on the SpeedyBee F405 WING, preset **Trainer · NAV LAUNCH**) gets, on the flight screen:
- **Launch** — bottom bar **Launch (L)** (bench function `launch`, default key L, Settings → Keys), enabled while
  connected; on a heli bench it is hidden and the key only logs "not a plane". It sends exactly
  `{"type":"launch"}`: the running scenario's `launch` defaults (the trainer: 50 km/h in 0.5 s at 10°). The **▾**
  popover's *override* adds `speed_kmh` / `duration_s` / `pitch_deg` (docs/protocol.md → `launch`; the core
  warns in the log below 22.4 m/s²). The core's `launch` events show in the Aircraft window (`launch  none · ramp…`
  → `released at 13.9 m/s, 0.61 m`) and in the log (`launch: ramp… 50 km/h at 27.8 m/s², pitch 10°`).
- **Aircraft window rows** from `state.plane` (see **Flight screen windows**): airspeed (m/s and km/h), α / β; thrust N,
  prop rpm, throttle; surface deflections (aileron L/R, elevator, rudder in degrees: + = aileron trailing edge down,
  elevator TE up, rudder TE right); launch phase (`none | align | ramp`), held / posed, the running scenario and its
  last step event (`pose 'launch attitude' (step 1 of launch)`, `scenario 'launch' ended`); a `mix_warning` (both
  ailerons the same way) stays as a row.
- **Crash** (plane and heli): the `crash` event opens the Aircraft window with a red banner `CRASH at t = … s: roll,
  pitch, sink` (heli: speed) and a **Reset aircraft** button; Reset aircraft (button, R) or the core's `reset` event
  clears it.
- **Log**: `beep` / `motor_beep` events (the beeper toggles every few hundred ms while armed) are collapsed: the
  first of a burst is logged, the rest of each 1 s window becomes one line `beep ×N more (t = a..b s, collapsed)`.
  A new connection (Restart core, reconnect) clears the previous core's crash banner, launch / step lines and
  config progress.
- **First start** (a new firmware + CLI script + calibration: ~40 virtual s, ~4 host minutes at the boot's
  ~0.11×): the core's control port answers at once (the 20 s wait is unchanged); after connecting, Status says what
  the core is doing instead of an error, and nothing times out: the firmware line `firmware: waiting for it to answer
  (booting; …)` while `fc.msp` is false (`busy with its first start` while configuring, `rebooting after the config
  save…`), and the config line — shown only while it runs — from the `config` events `config: applying the CLI
  script (13 lines)…` → `config: calibrating the accelerometer… pose 3/6 (left side down)` (`calibrating
  {"function":"calibrate_acc"}`, then the function's `step k/6` messages and the `pose` events with an iNav `index`:
  whichever is further) → `config: applied · ACC calibrated` (hidden 6 s later; `restored` on later starts, also when
  the core replays it to a late client; `FAILED — line N: …` / `FAILED — function calibrate_acc: …` stays). Each goes
  to the Log with the `config:` prefix (`applied with calibrate_acc, ACC calibrated`, a replay says so). Later
  starts restore the snapshot from the core's `cfg-cache` (default `<user cache dir>/flightbench/config`; the
  core takes `XDG_CACHE_HOME` when set).
- **Switches** — once the core has read the firmware's modes, the Transmitter window shows the switches derived
  from them (see **Firmware views**; the trainer: ch5 `— | ARM`, ch6 `— | ANGLE`, ch7 `— | NAV LAUNCH`, keys 5 / 6 / 7)
  and Status shows the active modes as tags. The trainer's channels are all `auto` (no `transmitter` block; its old
  `switches` names ARM / ANGLE / NAV LAUNCH are kept as labels of ch5..7, see **Benches → Transmitter model**), so
  before the firmware's modes arrive the Transmitter window has no switch row; `switchOrder` (one line) shows under the
  rows. Keyboard as the transmitter's switches: `switch_1..switch_5` (default keys **5..9**, Settings → Keys; binds
  stored under the older ids `switch_arm` / `switch_mode` / `switch_aux` still count) step the rows in channel order
  (a channel the radio drives is left alone and logged). Throttle from the keyboard: W / S (`stick_collective_plus` / `_minus`; an integrator that starts low and holds).
- **Visual** `models/trainer/visual/trainer.tscn` + `TrainerVisual.cs` (a placeholder: fuselage, 1.88 m wing,
  tail slabs, prop disc, a small FRD axis gizmo at the CG): pivots `aileron_l`, `aileron_r`, `elevator`,
  `rudder` on their hinge lines rotated by `state.plane.surfaces_deg` (a core without `plane`: `actuators` ×
  20°), `prop` spinning at `rpm`; `cg_height` 0.1, `view_size` 1.88. A bench's optional `visual` (path relative
  to the data root) names the scene instead of the `models/<aircraft>/visual/<aircraft>.tscn` convention. The
  cameras scale with `view_size` (Pad keeps ≥ 3× the model across, Chase backs off by span / 1.6: the RS7
  unchanged); a plane bench gets a 3 km floor with the grid fading at 500 m.
- **Machine**: the app starts `-M <machine>` from the session-check's launch line (`machine=speedybee-f405-wing`),
  `nexus-xr` when absent. Bench `props` may be numbers / booleans in the file (the preset's `accel_offset_mg:
  20`); a prop the manifest does not type goes into the session as a number / boolean when it reads as one.

Self-tests (`FB_SELFTEST=all` or by name; the runner opens the flight screen with the trainer preset in memory,
then an RS7 bench, both connected to the local fake core; messages are injected as if the core sent them):
`plane_state` (PlaneStatus on a `state.plane` sample + launch start/release, step, scenario_end, mix_warning,
plane and heli crash, reset events, the beep collapse — through `AircraftReadout`; on the screen: the Aircraft
window's rows, launch lines there + in the Log, crash banner, Reset aircraft clears it), `plane_visual` (the scene and `TrainerVisual` in the main view and the Model window,
the four pivots + prop, ±10° / 5° / −7° applied with the trailing edges moving the right way, prop spinning),
`launch_cmd` (button and key each send exactly `{"type":"launch"}`, the override adds the three fields, the
an all-auto bench without modes: no Transmitter row and switch_3 sends nothing; RS7: button hidden + disabled, the key sends nothing), `config_progress`
(ConfigProgress through the trainer's first-start sequence, applied shown then hidden, restored, failed stays; on the
screen the same events over seconds with a pause mid-calibration: the Status config line follows, still connected,
no error).

Checked 2026-09-29, live (see **Evidence: the plane** below).

## Firmware views (modes, functions, diff, OSD)
Product decisions 2026-10-01: the firmware's config is the firmware's — modes are read from it over MSP and never
written (the configurator does that), the bench's shipped diff is a starting point, calibrations and the launch are
**environment functions**, nothing in the release UI edits the config. The core sends (docs/protocol.md, `core-modes`
merged; checked against the packaged core 2026-09-29, see **Reconciliation with the real core**):
`{"type":"modes","variant":"RTFL"|"INAV","boxes":[{"id","index","name"}…],"ranges":[{"slot","channel","rc_channel","rx_channel","box","start_us","end_us","logic"}…],"rx_map":[…]}`
(or `"error":"cli"|"power_off"`), `{"type":"diff","text":…,"stale":bool,"source":"first_start"|"snapshot"|"request","at":…}`
(refused: the last text or null, `stale:true`, `error`), `{"type":"function","name","phase":"start|step|done|failed","step","steps","detail"}`,
`osd` in `hello` / `state` (`{"kind":"max7456"|"msp_displayport","cols","rows","video"}` or null), the manifest's
`aircraft[].functions` (`{"name","label","auto_first_start","params"}`), and to a late client the last `config` event,
`function` and `diff` with `replay:true`. The app sends `{"type":"subscribe","modes":true}` after every `hello`
(without it the core reads the modes only on request) and asks with `{"type":"modes"}`,
`{"type":"diff","refresh":false}` / `{"type":"diff"}`, `{"type":"function","name":…}`.
- **Switches** (`Modes.cs`, `Flight.Modes.cs`; docs/ui.md "Switches from the firmware's modes"): per aux channel with
  ranges, the span 900..2100 µs is cut at every range edge, each piece labelled with the boxes covering it (start ≤ µs
  < end; `+` joined, `—` for none), equal neighbours merged → 2, 3 or more positions; position *i* of *n* sends
  1000 + 1000·i/(n−1) when inside it (else the nearest of 1000 / 1500 / 2000 inside, else the middle). Transmitter:
  one row per switch (`ch6  [—] [ANGLE] [HORIZON]  [6]`), the current position highlighted (from the channel as sent),
  click or key **switch_1..switch_5** (defaults **5..9**: the N-th row in channel order, static and derived rows
  together — see **Transmitter**) → `RcInput.Override` on a channel whose source is `ui` or `none` (a channel on the
  radio: shown, not driven; a switch never starts on ARM unless the user put it there). Header
  `modes: from firmware (t = …)` / `from bench snapshot (…)`, **Update modes**. Every `modes` message is saved into the
  bench (`modes`, informational) and refreshes `switches`; a `modes` error (`cli`, `power_off`) is logged and the
  switches stay. The channel is the range's `rx_channel` (what the app sends in `rc`), never the firmware's
  `rc_channel` / `rx_map`. RotorFlight `logic:"and"`: a box is on where all its AND ranges on the channel cover the
  position, labelled with its other conditions — `ARM & ch8` (an AND slot on another channel), `RESCUE & ANGLE` (an
  AND slot linked to a box). Status shows the active modes as text tags (`fc.mode`, else `state.fc.boxes` named
  through the modes) in its firmware line. On a channel the bench makes a static switch (`switch2` / `switch3` /
  `switch6`) the modes only label its fixed positions (the boxes active at each position's µs), never rebuild it.
- **OSD** (Status sensors line): `OSD: MAX7456 30×16 (no video in FlightBench)` / `OSD: MSP DisplayPort 50×18` /
  `OSD: none` from `osd` in `hello` / `state`; nothing when the core does not say.
- **Functions** window (Bench → Functions…): one button per function of the bench's aircraft in the manifest
  (`aircraft[].functions` only: the RS7 `calibrate_acc`, `calibrate_gyro`; the trainer those and `launch`), enabled
  while connected → `function`, the progress line from the `function` messages (`running… 0/6`, `step 3/6: left
  side down (t = …)`, `done: …`, `FAILED: …`; cleared on a new connection; the core's replay to a late client sets
  it again, logged `(replay: …)`), **at first start** per function (default `auto_first_start`, kept in the
  bench's `firstStartFunctions`; informational — the protocol has no session field for it, the core decides from the
  aircraft). Launch (L) stays in the bottom bar and sends `launch` as before.
- **Config** window (Bench → Config…; was **Firmware config**): the firmware's `diff all` is its right half (DiffView: the text with its header — from the firmware / from the bench, first start / saved snapshot / on request, when, lines, `STALE` and why —, kept in the bench as `firmware_diff`; a refusal shows under the header and keeps the text; **Show cached** = `{"type":"diff","refresh":false}`; **Refresh from firmware (reboots)…** asks first and sends `{"type":"diff"}` only while connected and `fc.armed` is known false; **Copy to clipboard**); the rest — the script by layer, Changes, the operations — see **Config layers**.
- **Top bar**: left Menu · Bench · Windows · View · **Settings** · Copy configurator address, centre the bench title
  (`<bench> · <board> / <aircraft>`, a label: not clickable), right Disconnect. **Settings** opens the settings page
  in a window over the bench (`Settings.Overlay`); **< Back to the bench** closes it, the core stays connected; the
  bench's keys are not dispatched while it is open; key binds and channel sources apply when it closes.

Self-tests (the runner opens the flight screen once more with an in-memory RS7 bench, connected to the local fake
core, after the plane screens): `modes_switches` (a fixture with a 2-position ARM, a 3-position ANGLE / HORIZON given
high range first, overlapping ANGLE + NAV LAUNCH, a lone middle range, an odd range, junk and an empty one → positions,
labels, µs; AND slots; the ARM-safe start; active ids; the bench snapshot round trip; on the screen: the fake core's
modes live — it sends them only to a subscriber —, the fixture's 6 rows with keys 5..9, the switch_2 key cycles ch6, a
click drives ch7, the active modes as Status tags, Update modes sent), `functions_panel` (the aircraft's functions
only, buttons gated on connected, progress lines, first-start default and bench override; on the screen: the
windows, a click sends `function`; `modes_switches` also puts a mixed transmitter model on the screen —
ch5 derived ARM, ch6 `switch2` throttle on a channel the modes do not mention (`low | high`, its key 1000 → 2000 → 1000),
ch7 `switch3` over ANGLE / HORIZON ranges (1000 / 1500 / 2000 labelled `low | ANGLE | HORIZON`), ch8 `switch6` (the six
ArduPilot values by key), ch9 `auto` without ranges (no row), ch10 derived BEEPER on key 9: rows and keys in channel
order; then ranges moved onto ch6 relabel the static switch), `topbar` (the node order, the title not clickable, Settings over the bench
closed with Back: connected, no disconnect), `osd_line` (the kinds; `fc.osd` ignored; the Status sensors line from the
fake core's hello, then a state's `osd` + baro), `diff_view` (CRLF, header with source, stale + error,
`firmware_diff` round trip, read-only, Show cached when connected, Refresh from firmware only connected + disarmed
with the warning, the refusal line, Copy; on the screen the fake core's diff, an injected one, cached + refresh sent,
refused armed, a text-less refusal keeps the diff). The plane screens ignore the
fake core's `modes` (they test the screen without modes).

Evidence: `app/evidence/shots-2026-10-24/modes-switches-rs7.png` (plain fake core: Transmitter switches `— | ARM`,
`— | ANGLE | HORIZON`, `— | RESCUE`, the Status mode row and OSD line, the top bar), `app/evidence/shots-2026-10-24/modes-trainer.png`
(`fake_core.py --plane --manifest app/tests/manifest-fixture.json`: the trainer's switches, the Functions window, the
iNav configurator address `127.0.0.1:31235`, `OSD: MAX7456 30×16 (no video in FlightBench)`).

## Flight screen windows
Product decision 2026-10-01: **one fact in one place** (docs/ui.md → Flight screen windows).
- **Status** (`StatusPanel.cs`, compact, top-left) — four lines, nothing to press (one exception: **exit CLI
  (reboots)** next to the firmware line while it reads `in the CLI — MSP blocked`): the **link** (`connected ·
  realtime 0.39× · 97 states/s`; not connected: where, and the app's core when it matters), the **firmware** (`ARMED`
  / `disarmed` · the active modes as text tags `[ANGLE] [NAV LAUNCH]` · `flags: …` · `cpu N%`; while it does not
  answer: why — booting, first start, rebooting after the save —; `POWER OFF`; `in its CLI`), the **sensors** (OSD,
  `baro N m` from `fc.baro_alt_m` when the firmware reports it, GPS when the state has one) and the **config**
  progress only while it runs (applying → calibrating k/6 → applied, hidden 6 s later; a replayed `restored` the same;
  `FAILED` stays). No switches, plane block, crash banner, core pid / ports / address, Restart core or Bench check.
- **Transmitter** (right) — all input: the switches from the firmware's modes (header `modes: from firmware (t = …)`,
  positions, keys) and the bench's static switches (`ch6 throttle`), in channel order (see **Transmitter**), the sticks, the 16
  channels with the owner marks (`joy` / `kb` / `ui`: who last drove each), the hint line built from the bind table (`keys: Left/Right roll · Down/Up pitch · S/W collective (holds) · A/D yaw · switches 5 6 7 8 9 · launch L`).
- **Aircraft** (`AircraftPanel.cs`, below Status) — the model from the FDM / core state: altitude (`on the ground`),
  ground speed and climb, air data (plane: airspeed, α / β), motor (plane: thrust, prop rpm, throttle) or rotor (heli:
  head rpm, motor rpm, throttle; the core's state has no governor / collective field), surfaces in degrees (plane) or
  servos in % of travel (heli), launch phase / hold / scenario, the scenario's step, the mix warning, and the red
  **crash** banner with **Reset aircraft** (a crash opens the window). A row the state lacks is hidden.
- **Core**: the Bench menu's Core group (see **Core lifecycle**).
- **Layout**: the presets (and a bench without a saved layout) open Status (380×150 at 20,70), Aircraft (380×300 at
  20,270), Log / CLI (top centre, 420,70) and Transmitter (360×470 at 1220,390; the trainer's Model window above it);
  at 1600×900 nothing overlaps, and at UI scale 1.25 (1280×720 of UI) the fit moves only the right-hand windows.
  **Migration**: a saved layout without `Aircraft` (every bench before 2026-10-01) gets it opened where the Status
  window's plane block was — below the Status window, now compacted to 180 px, or, when that spot overlaps an open
  window (the RS7 presets' old Transmitter at 20,390), right of the Status window / right of the Transmitter / below
  the Log, the first free spot; logged `layout: … Aircraft opened at x,y …` and saved with the bench.

Self-tests (on the modes screen: an RS7 bench in memory against the fake core; the plane's rows on the plane screen):
`status_compact` (FcStatus lines and the config line's life; the Status window holds exactly link / firmware /
sensors / config and no control; the config line shows while it runs and hides on time, a replayed `restored`
briefly), `aircraft_window` (heli and plane rows, a sparse state hides rows; on the screens the heli's and the
plane's rows, the crash banner opens the window, Reset aircraft clears it), `transmitter_owns_switches` (no toggle in
Status; the switch rows in channel order and the key hints in the Transmitter window, no toggle there), `core_menu` (the Bench menu's Core group
in order, the Core line read-only, Restart core only for an own core, a bad address refused, nothing of the core in
Status), `layout_migration` (the old RS7 / trainer preset layouts → Aircraft placed off every open window, Status
compacted; the default and the presets' layouts: no overlap of Status / Aircraft / Transmitter / Log at 1600×900,
1280×720, 1536×864; on the screen a FlightWindows over the old RS7 layout), `core_fixtures` (the real core's
captures, next section).

## Reconciliation with the real core (2026-09-29, packaged core 87194cc)
Each item of the app-modes review checked against `core/qemu/evidence/modes/*.json` (captured from the real core)
and live against `dist-core/qemu-system-arm.exe`; the app was changed, the core never.

| item | real core | app |
|---|---|---|
| `functions` in the manifest | only `aircraft[].functions` (none on boards or top level) | **fixed**: board / top-level fallbacks dropped (`FunctionsFor(aircraft)`); captured manifest `app/tests/manifest-core-2026-09-29.json` |
| `hello.firmware` on a late firmware | the file name until MSP answers; then the log line `firmware says: …` (no `state.fc` field) | **fixed**: the configurator address follows `firmware says` (logged), and `modes.variant` before both |
| `variant` | `RTFL` / `INAV` in every `modes` | **fixed**: feeds the configurator address (`INAV` → `host:port`) |
| `logic:"and"` / `linked` | RotorFlight ranges carry `logic`; linked slots have `start_us`/`end_us` null | **fixed**: AND ranges labelled `A & B`; linked slots kept (for the labels), still no position |
| `rx_channel` | given for every channel range (RS7 AUX1 → 4 via `rx_map`) | matched (the channel the app sends; `rc_channel` / `rx_map` unused) |
| `subscribe {"modes":true}` | without it no automatic `modes` and no replay | **fixed**: sent after every `hello` (`CoreSession.SubscribeModes`) |
| `diff` reboots the firmware | `exit` after `diff all`; refused `armed` / `calibrating` / `cli` / `busy` / `not_ready` as a `diff` with the last text or null | **fixed**: Show cached (`refresh:false`) by default, Refresh from firmware asks + off while armed; refusals shown, the kept text stays |
| `diff.source` | `first_start` / `snapshot` / `request` | **fixed**: in the Firmware config header |
| `config` replay | the last `config` event with `replay:true` after hello | **fixed**: Status shows `config: restored (saved snapshot)` briefly; logged as a replay |
| `function` replay | the last `function` message with `replay:true` | matched (the Functions window's line), log marks the replay |
| `applied` | `{"functions":["calibrate_acc"],"acc_calibrated":true}`; `calibrating {"function":…}` with `function` steps | **fixed**: kept; the calibrating k/6 also from the function's steps |
| `modes` error | `{"type":"modes","error":"cli"}` | **fixed**: logged as not read (was "a message without boxes") |
| `status` message, `fc.osd`, top-level `boxes` | never sent | **fixed**: fallbacks dropped (`osd` from hello / state, `fc.boxes`) |
| `state.fc` | `msp`, `armed`, `mode`, `boxes`, `arming_flags`, `cpu_pct`, `cli`, `power`, `baro_alt_m` | matched (the Status firmware / sensors lines) |

`core_fixtures` feeds these captures through the app's models: the RS7 / trainer `modes` (variant, rx_channel,
logic → `ch5 — | ARM`; `ch5 ARM`, `ch6 ANGLE`, `ch7 NAV LAUNCH`), the trainer's first start (the nine Status config
lines from `config` + `function` steps, `applied` functions / acc_calibrated, the `first_start` diff), the second
session (replayed `restored` + snapshot diff, launch / gyro / acc function lines, three `calibrating` refusals then a
`request` diff), a function replay, the RS7's armed refusal without text and its `modes` CLI error, the replay
capture, and the captured manifest's functions. `tools/fake_core.py` now follows the same shapes (modes only to a
subscriber, refusals as `diff` messages, variant / rx_channel / logic, the plane's first start with `function` steps).

**Live check** (2026-09-29): the exported build copied to `%TEMP%\fblive` (outside the repo), empty `APPDATA`, temp
`LOCALAPPDATA` and `XDG_CACHE_HOME` (the core's cfg-cache went to `XDG_CACHE_HOME\flightbench\config`),
`FB_PRESET=<preset> FB_CORE_PORTS=31290 FB_LIVE_CHECK=1 FB_UI_SCALE=1`, 1600×900, the app-started bundled core:
- **RS7 · First flight**: `[live-check] PASS: Transmitter 'modes: from firmware (t = 10.21 s)' switches [ch5 (AUX1): — |
  ARM] (variant RTFL); Functions [calibrate_acc, calibrate_gyro] from live core; Firmware config 'diff all from the
  firmware (on request, t = 8.39 s, …), 43 lines'` (no diff kept at start: the check used Refresh from firmware, the
  firmware rebooted and the modes came again); Status `connected · realtime 1.00× · 101 states/s` / `disarmed ·
  [ACRO] · flags: BOOTGRACE CALIB · cpu 8%` / `OSD: none · baro 0.00 m`; configurator `tcp://127.0.0.1:31291`; `Core:
  running · pid … · control 31290, msp 31291`. `app/evidence/shots-2026-10-24/windows-rs7.png` (the layout),
  `app/evidence/shots-2026-10-24/windows-rs7-views.png` (+ Functions, Firmware config).
- **Trainer · NAV LAUNCH** (first start from an empty cfg-cache, ~2 host min): the config line through applying →
  pose k/6 → `applied · ACC calibrated`; `PASS: … switches [ch5 — | ARM; ch6 — | ANGLE; ch7 — | NAV LAUNCH] (variant INAV);
  Functions [calibrate_acc, calibrate_gyro, launch]; Firmware config 'diff all from the firmware (first start, t =
  39.91 s, …), 99 lines'`; Status `disarmed · [MANUAL] · flags: CAL · cpu 13%` / `OSD: MAX7456 30×16 (no video in
  FlightBench)`; Aircraft rows alt, speed, air, power, surfaces, launch, step; configurator `127.0.0.1:31291`.
  `app/evidence/shots-2026-10-24/windows-trainer.png` (taken right after `applied`: Status shows `firmware: rebooting after
  the config save…` and `config: applied · ACC calibrated`), `app/evidence/shots-2026-10-24/windows-trainer-views.png`.

## Cameras
View menu or keys **1–4**, saved in the bench (`camera`, with `bodyAxes`):
1. **Pad** — fixed where the pilot stands, 10 m behind the pad (south) and 1.7 m up, looking at the
   aircraft; the FOV narrows with distance (~5 m of view across the aircraft, clamped 10…60°).
2. **Orbit** (default) — mouse drag orbits, wheel zooms (0.8…200 m).
3. **FPV** — at the nose (`fpv` node), looking forward, following the body quaternion with a slight lag.
4. **Chase** — behind and above, following the heading with smoothing.

**Model** window (Windows → Model): a `SubViewport` with its own world and camera (front-right, eye
level, far ground plane as the horizon) showing the aircraft alone, centred, rotating with roll and
pitch; the **heading** check box adds the yaw. Footer: roll / pitch / heading from `euler_deg`.

## UI scale
Settings → Interface → UI scale: `auto` (default: screen DPI / 96, clamped to 1.0…2.0, rounded to
0.25) or a fixed 1.0 · 1.25 · 1.5 · 1.75 · 2.0; stored as `uiScale` in `user://settings.json`, applied
at startup by the `DevHooks` autoload and at once when changed (`Ui.ApplyScale`). Mechanism:
`display/window/stretch/mode="disabled"` + `GetTree().Root.ContentScaleFactor` — the root's 2D space
becomes window size / factor, so Controls, fonts (re-rasterized, sharp) and the embedded flight
windows scale, while the 3D view renders at the full window resolution. `canvas_items` is not used:
it would also scale by window size / base size. Flight windows are clamped into the visible area
(layouts are in UI units). Check boxes / check buttons / Tree checks use SVG icons (`DpiTexture`,
sharp at any scale) from `Ui.BaseTheme`, set on the root window and on the flight bars/windows.

## Transmitter
The app is the receiver: joystick axes/buttons → CRSF channels 1..16 (`rc` message), with calibration
and reverse, like a configurator's Receiver tab. Switch logic (arm, motor switch, modes) stays in the
radio's model, as on the real helicopter. RS7 firmware map `AECR1T23`: 1 roll, 2 pitch, 3 collective,
4 yaw, 5 ARM (1700..2100 = armed), **6 throttle** (the governor runs off it: high = throttle fully on, the head
spools; low = throttle hold), 7/8 modes.

**The Transmitter window is the radio** (decisions 2026-10-23), not the firmware's mode table: one row per channel
5..16 that has a switch, **in channel order** — a static switch of the bench's transmitter model (`switch2` /
`switch3` / `switch6`, always; see **Benches → Transmitter model**) or a switch derived from the firmware's modes on an
`auto` channel (once the firmware gives ranges there; **Firmware views → Switches**). Header: `modes: from firmware
(t = 10.19 s) · ch6 throttle: bench switch`. A row: `ch6 throttle  [—] [—]  [6]` — the positions as buttons (the
pressed one follows the channel as sent), labelled with the firmware's boxes active at that µs (`—` where none), the
key on the right. A static position's tooltip says its µs and the boxes active there. A row drives its channel when
the channel's source is `ui` or `none` (a click or its key → `RcInput.Override`); on a joypad channel it is shown, not
driven. A position is sent only on a click / key: a static switch starts at its `start`, a derived one where its
channel is, never on an ARM position nobody asked for. On the RS7 preset: ch5 `— | ARM` (key 5), ch6 `throttle`
`low | high` (key 6). A static position the firmware's modes do not name carries the type's own name (`low | high`,
`low | mid | high`, `1..6` = ArduPilot's FLTMODE1..6); `—` is left to derived switches, where it means "no box here". The old ARM / mode / third toggle row and the `ArmChannel` / `ModeChannel` / `RescueChannel`
settings are gone.

**Live check: take-off** (`FB_LIVE_CHECK=1` on a heli bench, after the views' PASS line): the take-off through the
real key path — synthetic key events through the bind table, as the `keys` self-test: key switch_1 → waits for the
core's `armed` event (20 virtual s), key switch_2 → waits for head ≥ 1600 rpm (≈ 90 % of the RS7's `gov_headspeed`
1800; 45 virtual s) and saves `<FB_SCREENSHOT>-spool.png`, then holds `stick_collective_plus` (**W**; `Input.ParseInputEvent`,
re-asserted every frame, through the bind table like a real key) until the altitude is ≥ 0.5 m (20 virtual s) →
`[live-check] PASS: take-off from the keyboard: armed at t = …, throttle on (key '6' → ch6 2000 µs), W held … (ch3 <from>
→ <to> µs, held by kb) → altitude … m at t = …, head … rpm; …; joypad …` + `<FB_SCREENSHOT>-airborne.png`; otherwise
`FAIL: take-off: <reason>; last state: …` (arming flags, ch5 / ch6 / ch3 µs, head rpm, altitude, rows, the joypad with
the raw axes of channels 1..4 and who holds them) and exit code 1. It passes with a joypad plugged in (the collective
starts from the pad's value, W takes the channel) and with `FB_NO_JOY=1` (from the key layer's low end). Evidence:
`app/evidence/tx-model-2026-10-23/` (keyboard only), `app/evidence/kb-layer-2026-10-23/` (both configurations).

**The keyboard is a layer** (decisions 2026-10-23): always on over channels 1..4, beside the joypad (not while a text
field has the keys, not while Settings / Edit bench is open over the bench), never a channel source. Its keys are the
**Sticks** group of Settings → Keys (`stick_roll_minus/plus` ← / →, `stick_pitch_minus/plus` ↓ / ↑,
`stick_collective_minus/plus` S / W, `stick_yaw_minus/plus` A / D), dispatched through the same table as every
function (`Keybinds.Match` → `HandleKeys` → `RcInput.StickKey`; the release of the same key / joypad button →
`StickRelease`; the window losing focus lets every held key go, and so does a held key Input sees released when its release
event went to a window without the relay — a top-bar menu, a dialog —, with a `[rc] … released outside the flight view` line), so a rebound stick key works from the 3D view and from
a focused embedded window alike. Roll / pitch / yaw: ±500 µs while held, centre on release; the collective is an
integrator (1 / s while held, the value holds; it starts at its low end). **Two hands on one channel: the last input that
changed it holds it** — a joypad source takes it when its value moves more than `RcInput.JoyTakeUs` (100 µs, a tenth of
the travel: a deliberate move, so a stick's slow drift never takes a channel from the keys; a stick resting inside the
deadzone maps to exactly the centre) from where the pad was when it last held it;
a key takes it when pressed, and when released while the keys still hold it (a pad that moved meanwhile keeps the
channel: the release changes nothing); the pad taking a channel from the keys logs `[rc] chN: the joypad took it from the
keys (moved a → b µs | the pad appeared)`; when a key takes the collective from the pad, the integrator starts from the
channel's value (W nudges from where the stick left it). A pad that appears (start, hot-plug, replug) takes its channels,
keys held or not (a development bench: a restart is cheap, no rule for it).
Nothing holds a channel → a Transmitter row's value if it has one, else on 1..4 the key layer at rest, else 1500. The
Transmitter window's bars mark who last drove each channel (`joy`, `kb`, `ui`: a row's key = `kb`, a click = `ui`);
the window's hint line is `Keybinds.Hint` (from the bind table, never a literal).

EdgeTX (TX16S): plug USB, choose **USB Joystick**; in classic joystick mode the model's CH1..CH8
arrive as axes 0..7. In Settings → Transmitter:
1. pick the radio in **Joypad** (re-found by name after a replug at another index);
2. **EdgeTX classic: CH1..CH8 → axes 0..7** (channels 9..16 = none, no reverse — EdgeTX sends CH2
   forward as +1, which is what the firmware wants);
3. **Calibrate**: move all sticks and switches to their extremes, centre the sticks, **Done** — writes
   min/centre/max into every channel that uses each moved axis;
4. check the live bars: stick forward → ch 2 high, collective up → ch 3 high, ARM switch → ch 5 > 1700;
5. the radio's switches then drive their channels themselves (source `axis n`): the Transmitter window shows those rows
   without driving them.
Settings → Transmitter starts with the joypad as a fact: `Joypad: XInput Controller — connected` or `Joypad: — not
plugged in (channels from the keys and the Transmitter window)`. (The `Sticks: Mode 1..4` picker beside the device stays:
it is the transmitter mode, which only places the sticks in the drawing — it never chose keyboard vs joypad.)
Per channel: source (`none`, `ui`, `axis n`, `button n`, `buttons n,m` = 3-position:
n low, m high, neither centre), **Detect** (the next axis moving by > 0.5 or button pressed becomes the
source), reverse, min/centre/max µs = what the source really sends at its ends and centre (mapped onto
1000/1500/2000 in two segments; deadzone around the centre for axes; the spin boxes keep min < centre <
max). A radio picked by name that is not plugged in means no joypad (the keys / centre), not another pad.
The `keyboard` source is gone: an older `settings.json` with `keyboard` on a channel reads as `none` (one log line,
`[settings] channel source ... reads as none`). The raw strip shows every axis
as the OS reports it. `ui` = the Transmitter window's switch for this channel (its type is set per bench: Edit
bench → Transmitter); `none` is driven by that switch too when the channel has one, and on 1..4 by the keys. There is
no substitution: without a joypad the keys are simply the only thing moving channels 1..4. Default without settings:
gamepad layout (axes 0,1,3,2 → 1..4, ch 2 reversed), 5/6/7 = `ui`, 8..16 = `none`. Sending: only on change, at most
every 4 ms. Joypad events are removed from the built-in `ui_*` actions so the sticks do not move the
GUI focus.

## Manifest (catalogue)
The core declares what it can emulate in its `manifest` (docs/protocol.md; also `core/qemu/run.sh
--manifest`): boards with their pads (`id` = firmware id, `label` printed on the board or null, `kind`,
`flags` such as `rx_only`/`tx_only`/`5v`/`usb`, `pins`, `hint`, onboard device: `onboard:true` + `device`,
`detachable`, `part` — `onboard:{device, detachable}` is read too; an older manifest's `slots` become
label-less pads), devices (`ports` with `kind`/`dir`/`optional`/`bundled` — none = one port of the old
`kind` —, `category`, `props`, `faults` with `params`, `signals`, `implemented`), aircraft with
`actuators`; optional `boards[].defaults` (suggested wiring, session-form entries) feeds Autofill.
`Manifest.Current` is the app's catalogue, from the first source that exists:
1. the live `manifest` (sent after `hello`) — the flight screen replaces the current one with it and
   caches it to `user://manifest.json`;
2. `FB_MANIFEST=path` (dev: an offline sample);
3. the cached `user://manifest.json` (the wizard and Settings have no core: they use this);
4. the built-in fallback = the catalogue the app hard-coded before (7 devices, their inspector fields,
   no faults; slots from `boards/<board>/*.config`, actuators from `models/<name>/README.md`).
A board / aircraft the manifest does not list falls back to the repo the same way.

## Wiring graph
`BenchData.Wiring` stays the data model; the graph is a view/editor of it. One entry per device
instance: its main wire (`port` on `pad`, the pad by firmware id), its other ports' wires (`links`:
port → pad, e.g. the ESC's `tlm` → `uart2`), `actuator` / `reversed` / `option` (= `telemetry`) and
the other props. Onboard devices are entries on their own pad; `detached` (bench) lists the onboard
devices the user removed.

- **Board nodes** (right), one input per pad of the manifest's board: `nexus-xr · ports` (top) = the
  external pads (everything without an onboard device), `nexus-xr · on board` (below it) = the pads
  that carry an onboard device (`gyro1`, the `i2c3` baro bus, the `uart5` ELRS receiver, `flash1`).
  The printed label first and the firmware id second (`PORT-A · uart4`); a pad without a confirmed
  label shows the firmware id and a hint in amber (`servo1 · S1?`, `motor1 · no label`); kind and flags
  on the right (`UART rx only`, `UART 5v`); tooltip: pins, bus, onboard part, what the core models
  there. No pads in the manifest → its slots; no board in the manifest → the board's unified config
  (`gyroN`, `i2cN`, `uartN`, `motorN`, `servoN`); no onboard pads → only the ports node.
- **Device nodes** (left) and the `+ device` palette: one output per device port, typed by its kind
  (ESC: `signal` motor in, `tlm` UART out, `rpm` capture out; gyro: `spi` with its bundled `int`
  shown on it; receiver: `uart`; servo: `pwm`), optional ones marked; props summary underneath.
  `implemented:false` devices are greyed "(not modelled yet)" and can still be wired.
- **Rules**: GraphEdit only joins equal kinds (the handler checks again); a UART port's direction must
  fit the pad (`in` needs the pad's TX, `out` its RX, `both` both; `rx_only`/`tx_only` from the flags or
  pins, `usb` carries both), so a receiver cannot go to the RX-only TELEM while the ESC's `tlm` can;
  one wire per port; a non-I2C pad takes one device (a new wire replaces the old one; an I2C pad takes
  several); a user device goes onto an on-board pad only when the pad is an I2C bus or its onboard
  device is detachable and detached. A refused drop says why next to the toolbar.
- **Onboard devices** (manifest `onboard`) appear pre-wired to their pad on the `on board` node, drawn
  grey with a badge: `soldered` (not detachable: no ×, disconnecting is refused, the tooltip says why)
  or `onboard` (detachable: disconnect, × or the inspector's **Detach from the board** removes it —
  nothing moves: the wire goes, the node stays where it was with the badge `detached`, and the
  inspector warns "onboard device detached: the firmware will not find it"; dragging its port back onto
  its pad or **Put back on …** restores it). An onboard device cannot be moved to another pad; its pad
  takes no other device while it is there (detach it first).
- **`assumed` badge**: entries the old-bench migration added because the core assumes them present
  (below); the toolbar counts them. Saving the bench turns them into ordinary entries.
- **Inspector**: the device's ports and where each is wired (bundled ones with the primary), its onboard
  state, then one field per manifest prop — `bool` check box, `enum` dropdown, `int`/`float` spin box
  within min/max (`default` = unset, the core's default), `actuator` dropdown of the aircraft's actuators.
- **Autofill**: the manifest's `boards[].defaults` when present, else the board-config rule (servos on
  the aircraft's actuators, the ESC on `motor`, gyro / baro / receiver), plus the board's onboard devices
  (an autofilled entry on an onboard pad becomes that device). The ESC's optional `tlm` / `rpm` stay
  unwired.
- **Session** (`ToSession`, Firmware → **Save session file…**): the new form, one entry per wire
  `{"device","port","pad"}` (pad by firmware id; `port` left out only when the manifest declares no
  ports), props on the entry's first wire (`actuator`, `reversed`, `telemetry`, the rest typed by the
  manifest); an onboard device only when it carries props; `{"device":…,"detached":true}` per removed
  onboard device.
- **Older benches** (`slot` entries, none with `pad`/`port`, no `detached`) are migrated on load to
  what the core runs for an old-form session (`docs/protocol.md`): `slot` → `pad` (a slot name is the
  pad's firmware id), `port` → the device's first port; onboard devices stay attached, except a
  detachable one the list omits (the gyro: the old `imu=off` rule) — the onboard receiver stays unless
  the list puts a `crsf` on a UART that models one (that one is then the receiver) or another device on
  its pad; soldered ones always stay. Then the devices the core assumes are added, marked `assumed`:
  the ESC on `motor1` (telemetry default `bidir`, actuator = the aircraft's `throttle`) unless an ESC is
  listed or `motor1` is taken; a servo on each free `servo1..4` with the aircraft's default actuator
  (manifest `fdm_inputs` in order, else `swash_a, swash_b, swash_c, tail`; empty when the list already
  drives it); a receiver on `uart5` when the board has no onboard one and the list names none. So the
  old smoke bench (gyro only) shows gyro + baro + receiver + flash + ESC + 4 servos = 9 wires. Saved
  again in the new form (no `slot`, no `assumed`). A list mixing entries with and without `slot` only
  gets `slot` → `pad`.
- **Layout / positions**: devices on the left in pad order (one column; more than 8 → two: the ports
  node's devices far left, the onboard ones next to the board at the `on board` node's height), the
  ports node top-right, the `on board` node below it; the view fits the content. `wiringLayout` in the
  bench (`board` = ports node, `onboard`, `device@pad` — a detached onboard device keeps its pad's key;
  old `device@slot` keys are the same), empty = auto-layout; Autofill and Re-layout clear it. A missing
  gyro shows a warning next to the toolbar (not blocking).

## Faults
Windows → **Faults** (layout saved like the others): one row per fault of the manifest, grouped by
device — the devices present on the bench first (wired, or onboard and not detached), the rest under a
collapsed **other devices** header, greyed and disabled with "not wired" (the core rejects a fault for a
device the session has not wired; the header opens by itself when one of its faults is active). A row: toggle → `{"type":"fault","device":..,
"kind":..,"on":..}`, with `params` when turning on (required int/float params: slider, e.g. link
quality % for `crsf.lq`; optional ones: check box + value, sent only when checked); moving a param
while the fault is on sends it again. The row is **lit while the core lists the fault in
`state.faults`** (the toggle follows the core too, e.g. a fault switched by another client). The
keybind cell shows the fault's bind. `event fault` lines go to the log with their data
(`event fault {"device":"crsf","kind":"lq","on":true}`). Faults of a not-modelled device are disabled.

## Keys
Settings → **Keys** lists every bind, in three groups:
- **Sticks** — the keyboard layer over channels 1..4 (see **Transmitter**): `stick_roll_minus` / `stick_roll_plus`
  (Left / Right, ch1), `stick_pitch_minus` / `stick_pitch_plus` (Down / Up, ch2), `stick_collective_minus` /
  `stick_collective_plus` (S / W, ch3; the label says it is an integrator: the value holds), `stick_yaw_minus` /
  `stick_yaw_plus` (A / D, ch4). Channel order = the Transmitter window's (the firmware's map is not read).
- **Transmitter switches** — `switch_1..switch_5` (5..9: the N-th row of the Transmitter window — channel order, static
  and derived rows — to its next position; a key without a row logs `switch_N: no row N …`). Opened over a bench, each
  row says what it is there: `Switch 1 (ch5 ARM)`, `Switch 2 (ch6 throttle)` (the row's label, else its firmware boxes);
  plain `Switch N` from the main menu.
- **Application** — restart bench (F5), pause/run (Space), step (N), battery (B), reset aircraft (R), screenshot (F12),
  cameras 1–4, body axes (X), launch (L), each window toggle (unbound), one entry per manifest fault
  (`fault: crsf link_lost` …, unbound).
An explicit bind wins over a default on the same key (an older file may have put a function on A / D / W / S / an arrow). **Bind**, then press
a key (modifiers count: `Ctrl+F9`) or a joypad button (`Joy button 3`, any pad — a radio switch in
USB-joystick mode can trigger a fault); Escape cancels; a bind already used moves to the new function.
**Clear** unbinds. Stored in `settings.json` → `keys` by function id (`restart_bench`, `camera_fpv`,
`window:Faults`, `fault:crsf.link_lost`); a missing id = its default, `""` = unbound. The flight screen
dispatches every key and joypad button through this table, the stick keys included (only Escape is
fixed: it leaves a text field) — from the 3D view and from a focused embedded window alike (`InputRelay`: keys the window's GUI
did not take). Binds appear in the bar buttons (`Reset aircraft (R)`), the Windows/View menus and the
Faults rows.

## Signals
Every value in the `state` stream is a signal (`euler_deg.roll`, `pwm_us.1`, `fc.cpu_pct`, `on_ground`
as 0/1…). The core adds its own list (`signals`: raw sensor inputs, decoded outputs, governor…);
checking one of those sends `subscribe` and the values arrive as `samples`. Checked signals are
plotted together on the virtual-time axis (window 2 s … all; per-trace autoscale or one shared scale;
freeze) and can be recorded to CSV (`user://signals_<date>.csv`, one row per state tick, columns =
checked ids). A checked signal keeps the last 5 minutes at 100 Hz; the others only their last value (and
the state is not even flattened while the window is closed and nothing is checked or recorded). Virtual
time going backwards (a restarted core), `hello` and a new connection clear the data; a CSV being
recorded gets a `# restart: …` line there.

## OSD
The firmware's MSP DisplayPort canvas (`osd` messages: `rows`×`cols`, `clear`, `font`, `cells` of `{r,c,s,attr}`)
goes into one `OsdBuffer` (default 18×50; other rows/cols in a message resize and clear it; each string is
written from (r,c) onward, clipped at the row end). Two views draw the same buffer:
- **OSD window** (Windows → OSD; layout saved like the others);
- **OSD overlay** over the **FPV camera**, as in goggles: full width between the top and bottom bars,
  shown only while the camera is FPV and View → **OSD overlay (FPV camera)** is on (saved in the bench as
  `osdOverlay`, default on).

**Font per firmware.** The two firmwares index different fonts: the message's `font` (docs/protocol.md; the core
asks the firmware MSP_FC_VARIANT as a VTX does) picks the atlas:

| `font` | file | glyphs | glyph index of a cell |
|---|---|---|---|
| `rotorflight` (and absent / unknown) | `assets/osd/default.mcm` (RotorFlight Configurator) | 256 | the byte (`attr` bits 0–5 are a configured font page, see below) |
| `inav` | `assets/osd/inav-default.mcm` (INAV Configurator 9.1.1 `resources/osd/analogue/default.mcm`) | 512 | `(attr & 3) << 8 \| byte` — page 1 holds the HUD: horizon 0x14C..0x154, sidebars 0x131, crosshairs 0x13A 0x166 0x13B, logo |

Provenance (source URL, tag / commit, sha256, license) of both: `assets/osd/FONTS.md`. A message without `font`
keeps the current one (the core leaves it out while it does not know the firmware yet): RotorFlight's until a
message names another, and a new connection goes back to it. A change bumps the buffer's version, so the window and the
overlay redraw the kept canvas in the new font at once (no wipe). iNav's codes 0x60..0x7E are symbols, not lower
case, and upper case / digits coincide with RotorFlight's, so an iNav canvas drawn with the wrong font shows its text but
wrong symbols and a row of `T`s for a horizon (0x154 → 0x54). Evidence: `app/evidence/shots-2026-10-24/osd-inav-canvas.png` — the
core's captured canvas of the trainer on the pad (`tests/osd-inav-boot-canvas.txt`: ACRO, 16 sats, heading 359°,
RSSI 88, speed / altitude beside the horizon, DARM, 0.27 A, 16.7 V, 2 mAh, `THROTTLE IS NOT LOW`, on-time 00:29)
rendered with `OsdFont.RenderBuffer` in iNav's font; `app/evidence/shots-2026-10-24/osd-inav-window.png` — the flight
screen's OSD window against `fake_core.py --plane --osd` (the same layout, fake values).

**Frames.** `frame` (the device's DRAW count) is not used: the core skips the DRAWs that changed nothing (iNav
redraws ~47/s on the pad, ~5 messages/s reach the app), so the numbers jump, and each message is applied as it comes —
a delta onto the canvas, a full frame after a wipe. Nothing waits for a missing number; a still OSD simply gets no
messages. (Checked by the `osd` self-test: gaps, a number going back, a message without one.)

Drawing: cells are 2:3 (a 12×18 analog glyph); the grid is scaled to fit its control and centred;
transparent background. Glyphs come from a MAX7456 **MCM symbol font** (`scripts/OsdFont.cs`), decoded
once at first draw (~6 ms for 256 glyphs) into a cached glyph atlas: every code of the font is its glyph
(black + white pixels, transparent elsewhere; symbols: battery, arrows, crosshair, units).
Nearest filtering once a cell is ≥ 12 px wide, linear below. `s` has one char per cell: the firmware's
byte as a code point U+0000…U+00FF (non-ASCII JSON-escaped `\u00NN`), which the JSON decoder turns into
that char = the glyph index. `attr` = the MSP_DP_WRITE_STRING byte (`docs/protocol.md`). RotorFlight 4.6:
bits 0–5 = font page — page 0 is the loaded MCM, other pages are drawn with the same font and logged once
(`[osd] font page N not loaded`); `0x40` = blink; `0x80` (version flag) ignored; there is no invert bit. iNav 9.1:
bits 0–1 = bits 8..9 of its character code (0 or 1 seen), part of the glyph index, not logged; it never sets blink.
Blink = 2 Hz (250 ms shown / 250 ms hidden) phased from the cell's last write: with
`displayport_msp_use_device_blink` OFF (default) the firmware blinks the element itself (drops it every
other 250 ms phase and re-sends it with `0x40`), each re-send restarts the phase, so our shown window is
the firmware's and nothing double-blinks (a free-running toggle could sit in anti-phase and hide it);
with device blink ON the cell is sent once and we toggle it from there. `"link": false` on an `osd` message
(the firmware stopped drawing: power off, CLI mode, `link_lost` fault, contact lost) keeps the last canvas,
dims it to 35% and draws **NO SIGNAL** centred in the OSD font, in the window and the overlay alike; the
next message with `link` true/absent (a full frame) restores it. MCM as the configurator's parser reads it (`src/js/tabs/osd.js` `FONT.parseMCMFontFile`): line 1
`MAX7456`, then one byte per line as 8 `0`/`1` chars; 64 lines per character, the first 54 bytes = 12×18
pixels, 4 per byte, 2 bits each MSB first, row-major (the last 10 bytes are padding); `00` black, `10`
white, `01`/`11` transparent; 256 characters (RotorFlight) or 512 (iNav; read up to the font's capacity, like the
configurator).
- **Fonts:** `assets/osd/default.mcm` — the Rotorflight Configurator's default font (`resources/osd/2/default.mcm`);
  codes 0xA0 and up hold the Betaflight logo tiles. `assets/osd/inav-default.mcm` — the INAV Configurator 9.1.1
  analogue default font, 512 glyphs, byte-identical to the release (sha256 in `FONTS.md`). Both GPL-3.0
  (`assets/osd/NOTICE`, `FONTS.md`).
- **Another font:** drop any configurator `.mcm` of the same layout (RotorFlight `resources/osd/2/*.mcm`: bold,
  clarity, digital, impact, large, vision, …; iNav `resources/osd/analogue/*.mcm`) over the file, or set
  `OsdPanel.FontPath` (read with the capacity of the buffer's current font). An exported build must include `*.mcm`
  in the export preset's non-resource files filter (it does).
- **Fallback** (no font file / bad file: a warning, then text; also for codes outside the font): a monospace
  system font (Consolas / DejaVu Sans Mono / …), each glyph centred in its cell, white with a dark outline, printable ASCII as is (iNav: upper case,
  digits and plain punctuation; its symbols and page 1), other codes as `▯`.
`tools/fake_core.py` sends a `clear` on connect and then, every 100 ms, only
the changed cells of a RotorFlight-like layout (mode, battery, head speed, crosshair, timer, a
blinking `ARM` while disarmed), each with `"font":"rotorflight"` and a skipping `frame`. `fake_core.py --plane --osd`:
the trainer with an `msp_osd` (`hello.osd` MSP DisplayPort 50×18) and an iNav-like canvas — mode, sats, heading,
RSSI, speed, altitude, battery, fly time, and page-1 cells (attr 1): the horizon following the pitch, its sidebars,
the crosshairs — without `font` until the firmware "answers" (t = 3 s), then `"font":"inav"`; a full frame every 1 s,
deltas between, `frame` jumping by several.
- **`osd_font`** self-test (the flight screen against the plain fake core, with the modes tests): the fake core's
  messages name `rotorflight`; its stream stopped (`subscribe` `osd:false`), the captured iNav canvas is injected with
  `font:"inav"` → the OSD window's panel draws it (its drawn version = the buffer's, font inav, every non-blank cell
  from the atlas, none from the fallback); deltas at frame +5 (the horizon one row up) and +31 without `font` → applied
  and redrawn, font kept; `font:"rotorflight"` mid-session → the kept canvas redrawn in RotorFlight's font; the stream
  back on.

## Next
- Export: app icon for the exe. Presets: a zip of `dist/FlightBench` as the client download; the official RotorFlight release binary in `firmware/` once MSP on USART1 is confirmed to come from the custom defaults alone (`firmware/FIRMWARE.md`).
- Faults: timed faults (on for n seconds), a fault's bind as hold-to-apply.
- Signals: the manifest's per-device `signals` as groups in the list.
- First run against the real core once it speaks the protocol (`core-protocol` step): bench from the wizard → flight screen → arm from the keyboard → lift-off.
- Aircraft visual: servo geometry (arm length, ball radius, CCPM azimuths) from the board profile's mixer instead of constants.
- Signals: save the checked set into the bench; cursor readout on the plot; table mode with history.

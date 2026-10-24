# Evidence: the app/README screenshots, retaken on the v0.1.0 build (2026-10-24)

The early development-log screenshots of `app/README.md` (2026-09-28..30) were taken on the development machine and
showed its folders; this set retakes each of them on the v0.1.0 build (`FlightBench.exe` from the full export, every
preset, unpacked to a neutral folder, empty `APPDATA`, benches in `FB_BENCH_DIR`, core ports from 31296, the Xbox pad
plugged in). Nothing was clicked: every frame comes from the dev knobs in `app/README.md` → *Run*.

| file | what it shows | how (env / core) |
|---|---|---|
| `preset-menu-first-run.png` | the first-run main menu: three presets, no benches yet | `FB_NO_JOY=1`, empty benches folder, frame 400 |
| `modes-switches-rs7.png` | RS7 preset against the fake core: Transmitter switches `— | ARM`, `low | HORIZON`, `— | RESCUE`, Status mode row and OSD line | `FB_PRESET=rs7-first-flight FB_CORE=127.0.0.1:31265` (`fake_core.py`) |
| `bench-cli-indicator.png` | the firmware in its CLI: Status line, the Log / CLI field, exit CLI | same + `FB_CLI_SEND=status` |
| `bench-delete-dialog.png` | main menu → Delete…: the bench file and what goes with it | `FB_DIALOG=delete` (a Trainer bench selected) |
| `bench-wizard-script.png` | Edit bench → 5 · Summary of Trainer · NAV LAUNCH: firmware found, wiring, no bench lines | `FB_SCREEN=wizard FB_WIZARD_EDIT="Trainer · NAV LAUNCH" FB_WIZARD_STEP=5` |
| `config-layers-wizard.png` | the layered first-start script of a bench with a pasted config line, a disabled device line and two bench lines: 2 conflicts, the winning bench line selected | `FB_WIZARD_EDIT=trainer-dev FB_WIZARD_STEP=5 FB_WIZARD_SCROLL=700 FB_WIZARD_LINE="set small_angle = 25"` (session-check by the bundled core) |
| `config-window-script.png` | the Config window, Script ↔ Firmware, no diff yet | `FB_BENCH=trainer-dev FB_CORE=127.0.0.1:31266 FB_CONFIG=script` (`fake_core.py --plane`) |
| `config-window-changes.png` | the Config window, Changes: changed / only in the firmware / only in the script | same with `FB_CONFIG=changes` |
| `modes-switch-dash.png` | a plane bench's Transmitter rows with the "—" position pressed | `FB_BENCH=trainer-dev FB_CORE=127.0.0.1:31266 FB_WINDOWS=Transmitter,Status` |
| `modes-trainer.png` | the trainer's switches, the Functions window with the first-start calibration running, Status | `FB_CORE=127.0.0.1:31267` (`fake_core.py --plane --manifest app/tests/manifest-fixture.json`), `FB_WINDOWS=Transmitter,Functions,Status` |
| `osd-inav-window.png` | the OSD window against the fake plane core with OSD | `FB_CORE=127.0.0.1:31268` (`fake_core.py --plane --osd`), `FB_OSD=1` |
| `osd-inav-canvas.png` | the core's captured iNav canvas (`app/tests/osd-inav-boot-canvas.txt`) rendered in iNav's font by the `osd` self-test | `FB_SELFTEST=osd FB_OSD_PNG=<folder>` (Godot, headless, from the repo) |
| `windows-rs7.png`, `windows-rs7-views.png`, `rs7-liftoff.png` | the RS7 live check on the bundled core: the layout once the firmware answered; Functions + Config with the real diff; the take-off from the keyboard at 0.56 m (ARMED, 1669 rpm, ch3 held by `kb`) | `FB_PRESET=rs7-first-flight FB_LIVE_CHECK=1` (2 PASS lines, `windows-rs7.log`) |
| `windows-trainer.png`, `windows-trainer-views.png`, `plane-config-progress.png` | the Trainer · NAV LAUNCH live check on the bundled core: the first start (board script, ACC calibration poses), the layout, the views; the calibration at pose 3/6 | `FB_PRESET=inav-trainer-launch FB_LIVE_CHECK=1 FB_SCREENSHOT_CONFIG=…` (`windows-trainer.log`) |
| `osd-inav-live-trainer.png` | the Trainer · FPV with OSD bench live: iNav's own OSD in its font once the firmware answers | `FB_BENCH="Trainer · FPV with OSD" FB_LIVE_CHECK=1 FB_OSD=1` (`osd-inav-live-trainer.log`) |
| `config-layers-live-trainer-osd.png` | the same bench's layered first-start script from the bundled core: real provenance, the disabled device line, the winning bench line | `FB_SCREEN=wizard FB_WIZARD_EDIT="Trainer · FPV with OSD" FB_WIZARD_STEP=5 FB_WIZARD_SCROLL=900 FB_WIZARD_LINE="set nav_fw_bank_angle = 42"` |
| `plane-launch.png` | the trainer launched: the app flies the preset (`FB_RC_OFF=1`), `flight.py --plan plane-launch --t0 55` on the core's pilot socket, the frame at 10 m | `FB_PRESET=inav-trainer-launch FB_RC_OFF=1 FB_SCREENSHOT_ALT=10` + the pilot script (`plane-launch.log`, `plane-launch-pilot.log`) |

The `trainer-dev` bench of the config shots is the Trainer · NAV LAUNCH preset with `benchCli: ./trainer-dev.cli`
(`set small_angle = 25`, `set nav_fw_launch_climb_angle = 20`), `disabledLines: ["set gps_ublox_nav_hz = 10"]` and
`boardConfig: set nav_fw_bank_angle = 40`; the Trainer · FPV with OSD bench of the live shots has
`./Trainer___FPV_with_OSD.cli` (`set nav_fw_bank_angle = 42`) and the same disabled line.

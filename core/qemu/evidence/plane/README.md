# Phase 3: stock iNav 9.1.0 flies the trainer plane on the speedybee-f405-wing (2026-09-29)

> **2026-10-24:** the flight models were rewritten from first principles (`docs/fdm.md`); the flights and numbers
> below were made with the earlier models. The current baseline hashes are in `docs/contributing.md` (gates table).

Branch `plane-flight`, homelab (i7-4790), QEMU 11.1.1 + this tree built in `~/flightbench-spike/qemu-prof`
(`core/qemu/build.sh`), firmware the release hex `firmware/speedybee-f405-wing/inav_9.1.0_SPEEDYBEEF405WING.hex`
(unmodified; sha256 in `firmware/FIRMWARE.md`), bench `core/qemu/sessions/trainer.json` with the ports moved to
31280/31281 (HMP 31282, motion 31283, pilot 31284). Driver: `core/qemu/scripts/plane_evidence.py`
(stages C, L in one run, G, P in a second; each run starts with an empty `cfg-cache` and fills it first).

| file | what |
|---|---|
| `plane-evidence-CL-2026-09-29.log` | stage C (first start: script, save, ACC calibration, save; `status`, `get acc`) and L (plane-launch twice, report) |
| `plane-evidence-GP-2026-09-29.log` | stage G (ground take-off, link lost) and P (profile) |
| `plane-launch-2026-09-29.csv` | the plane-launch FDM log (every 10 ms, to 54 s; run 2 was byte-identical) |
| `ground-takeoff-2026-09-29.csv` | the ground-takeoff FDM log |
| `configurator-2026-09-29.log` | stage M (the iNav Configurator's connect on the MSP port; `plane_evidence.py M`) |
| `plane-launch-prof-2026-09-29.json`, `.txt` | `prof=on` profile of the plane-launch flight, `prof.py show` |
| `configurator-defaults-2026-09-29.log` | branch `inav-defaults`: stage D (what the iNav Configurator 9.1.1 sees after the connect) on the old and the new `inav.cli`, stage C and L with the new one, RS7 gate |
| `diff-first-start-preset-2026-09-29.diff.txt` | the firmware's `diff all` saved at that first start |
| `stage-e-2026-09-30.log`, `configurator-flight-2026-09-30*.log` | branch `bench-state`: stage E (the configurator through a flight; with a "Save and Reboot" armed / disarmed), the replayed configurator's own log |
| `plane-launch-preset-2026-09-29.csv` | the plane-launch FDM log with the new `inav.cli` (baseline `4f8c7114358a` until the sensors merge; superseded, below) |

The FDM log columns (`hw/arm/fb_plane.c.inc`): the FDM's truth (height, climb, airspeed, α, β, attitude, rates,
position, body specific force, motor rpm, ESC throttle, the ESC's pulse in µs, thrust, the servo µs by actuator
after the wiring = `in1..4` aileron_l, aileron_r, elevator, rudder, the surfaces in degrees, ground/held/launch)
and what the core polls from the firmware over MSP (`fc_armed`, `fc_mode` = iNav's active boxes, `fc_flags` =
its arming-flag word, `fc_roll/pitch/yaw` = `MSP_ATTITUDE`, iNav's pitch + = nose down).

## Configuration and ACC calibration (stage C)
First start, blank config sector: the CLI script (13 lines: `boards/speedybee-f405-wing/defaults.cli` +
`models/trainer/inav.cli`) at t = 7.94..8.19 s, `save`, reboot at 9.27; the core runs
`models/trainer/env/acc-calibration.json` on the FDM:

| pose (iNav position) | MSP_ACC_CALIBRATION at | collected at | MSP_CALIBRATION_DATA positions |
|---|---|---|---|
| level (0) | 14.791 | 15.334 (0.54 s) | 0x01 |
| inverted (1) | 18.791 | 19.351 (0.56 s) | 0x03 |
| left side down (5) | 22.791 | 23.349 (0.56 s) | 0x13 |
| right side down (4) | 26.791 | 27.346 (0.56 s) | 0x17 |
| nose up (2) | 30.791 | 31.303 (0.51 s) | 0x1f |
| nose down (3) | 34.791 | 35.324 (0.53 s) | 0x3f |

At the scenario's end (39.79) the firmware reports **acczero 41 41 41, accgain 4075 4075 4076**; CLI `save`
(39.89), reboot, the sector becomes the snapshot (memory + `cfg-cache`, 131072 B), event
`config {"state":"applied","acc_calibrated":true}` at 40.96. The bench's IMU has a static error of 20 mg on
every axis and +0.5 % sensitivity (`icm42688p` props `accel_offset_mg`/`accel_scale_pct`): 20 mg × 2048 LSB/g
(ICM42605 driver acc_1G) = 41, 4096 / 1.005 = 4075.6. After the reboot, CLI (t ≈ 47.5):
`Arming disabled flags: THR CLI` (the throttle channel at its 1500 µs default, and the CLI itself), `get acc`:
`acczero_x/y/z = 41`, `accgain_x/y = 4075`, `accgain_z = 4076`. No `ACC`; `CAL` clears ≈ 7 s after the boot
(iNav's gravity/gyro calibration), so in the flights the blockers are empty from t = 6.63 s.

Two things were needed for an honest calibration, both on our side (the firmware is stock):
- **Poses 3° off the axes.** With exactly axis-aligned poses iNav's scale solver (`common/maths.c`
  `sensorCalibrationSolveForScale`: a 4×4 Gauss elimination without pivoting whose first pivot is the X
  component of the Z-axis samples) divides by ~0; its check `isnan(x) && isinf(x)` is never true, so accgain
  became 0 and the saved config failed `settingsValidate` → `SETTINGFAIL` blocker. Replaying iNav's solver in
  float32 (the same elimination order): exact poses → NaN; 1–8° off → 4096 ± 0.01 %. A hand never holds a
  model exactly, so the scenario now tilts each pose 3°.
- **A sensor error to calibrate out.** With an ideal accelerometer the correct result is acczero 0/0/0,
  accgain 4096/4096/4096, which iNav's `accSetCalibrationValues()` (`sensors/acceleration.c:651`) treats as
  *not calibrated*: `ACC` came back after the reboot. Real chips have an offset and a sensitivity error; the
  trainer bench now gives the model one (default 0 everywhere else: RS7 unchanged), and the calibration
  measures it exactly.

The snapshot key covers firmware + script + calibration scenario + the IMU error, so the calibration runs once
per combination (~40 virtual s, ~4 host min: the first boot on a blank sector runs at ~0.11×).

## Arming and NAV LAUNCH (stage L, `flight.py --plan plane-launch`, T0 = 20 s)
Plan (pilot socket, every step with an exact virtual `at`): 20.0 ANGLE (ch6) + NAV LAUNCH (ch7) on — iNav
enters launch mode only from a box turned on while disarmed (`navigation.c:4686`) — 21.0 ARM (ch5, throttle
low), 23.0 throttle 1700, 24.0 `launch` (50 km/h, 0.5 s, 10°; the default scenario already holds the model 10°
nose up from t = 1 s and its `launch` step takes the command), 34.0 NAV LAUNCH off, throttle 1550, sticks
centred to 54 s.

- **Armed**: 21.08 s, blockers empty, mode `ANGLE+NAV LAUNCH` (control server, `MSP2_INAV_STATUS`; the CLI
  is not reachable while armed: iNav skips non-MSP bytes on MSP ports then).
- **Launch**: ramp 24.000..24.490 s, IMU specific force X 29.48 m/s² (= a 27.78 + g sin 10°; iNav's threshold
  18.63), released at 24.50 s at 13.89 m/s, 0.61 m.
- **Detection**: the motor starts at **24.570 s** (ESC pulse 125 → 138 µs), i.e. `nav_fw_launch_motor_delay`
  500 ms after a detection at **24.070 s = ramp start + 70 ms** (40 ms `nav_fw_launch_detect_time` + the
  15 Hz accelerometer filter). iNav exposes no "detected" flag over MSP (all launch states map to
  `MW_NAV_STATE_NONE`), so the detection time is read from the motor start; spin-up to 208.6 µs by 24.66
  (`nav_fw_launch_spinup_time` 100 ms; ≈1700 µs on ONESHOT125 = 212.5 µs at 25.0).
- **Glide before the motor**: 0.07 s, airspeed min 13.61 m/s.
- **Climb**: plateau 27.4..30.3 s at **16.8° FDM pitch (iNav: 17.3° nose up; target `nav_fw_launch_climb_angle`
  18°)**, **4.30 m/s** climb at 13.7 m/s, motor ≈ 9 990 rpm; height 28.4 m at 34 s. iNav ends the launch
  itself at 32.77 s (mode `ANGLE`), easing the pitch down.
- **Level flight** (ANGLE, sticks centred, throttle 1550 = 192.5 µs; 37..54 s): roll 0.0 ± 0.0°, pitch
  −0.4 ± 0.4°, airspeed 13.3 ± 0.2 m/s, climb 0.20 ± 0.07 m/s, heading 0.0 → 0.0°; iNav's attitude vs the FDM
  within 0.0° roll / 0.3° pitch.
- **Determinism**: two runs, FDM log to 54 s (5400 rows at 10 ms): **`1e4b3b911e52` both, byte-identical**
  (superseded by `4f8c7114358a` when `inav.cli` became the configurator's airplane preset, below)
  (the plan uses exact virtual-time deadlines; the `time step` stop jitter of phase 1 does not enter).
- Speed: 84 host s for the whole 54 virtual s (0.64×, boot from the snapshot included).

| t s | height m | climb m/s | airspeed m/s | roll ° | pitch ° | motor rpm | motor µs | ail L/R µs | elev µs | rud µs | armed | mode | iNav pitch ° (nose up) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 20.50 | -0.00 | -0.00 | 0.0 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1447 | 1500 | 0 | ANGLE | 10.1 |
| 21.50 | -0.00 | -0.00 | 0.0 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1447 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.2 |
| 24.00 | 0.00 | 0.01 | 0.0 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1540 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.4 |
| 24.07 | 0.01 | 0.34 | 2.0 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1539 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.4 |
| 24.25 | 0.15 | 1.21 | 7.0 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1539 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.5 |
| 24.50 | 0.61 | 2.42 | 13.9 | 0.0 | 10.0 | 0 | 125.0 | 1500/1500 | 1539 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.5 |
| 24.57 | 0.78 | 2.56 | 13.6 | 0.0 | 9.7 | 130 | 138.0 | 1500/1500 | 1540 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.5 |
| 24.66 | 1.01 | 2.45 | 13.1 | 0.0 | 9.0 | 5754 | 208.6 | 1500/1500 | 1548 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.2 |
| 25.00 | 1.75 | 1.95 | 13.4 | 0.0 | 6.7 | 9968 | 212.5 | 1500/1500 | 1574 | 1500 | 1 | ANGLE+NAV LAUNCH | 7.3 |
| 26.00 | 3.88 | 2.71 | 14.5 | 0.0 | 9.4 | 9994 | 212.5 | 1500/1500 | 1588 | 1500 | 1 | ANGLE+NAV LAUNCH | 10.7 |
| 27.00 | 7.31 | 4.02 | 14.3 | 0.0 | 14.7 | 9993 | 212.5 | 1500/1500 | 1574 | 1500 | 1 | ANGLE+NAV LAUNCH | 15.8 |
| 28.00 | 11.59 | 4.39 | 13.9 | 0.0 | 16.8 | 9983 | 212.5 | 1500/1500 | 1567 | 1500 | 1 | ANGLE+NAV LAUNCH | 17.5 |
| 29.00 | 15.94 | 4.31 | 13.6 | 0.0 | 17.0 | 9976 | 212.5 | 1500/1500 | 1571 | 1500 | 1 | ANGLE+NAV LAUNCH | 17.4 |
| 30.00 | 20.21 | 4.20 | 13.4 | 0.0 | 16.7 | 9959 | 212.4 | 1500/1500 | 1564 | 1500 | 1 | ANGLE+NAV LAUNCH | 17.1 |
| 31.00 | 24.06 | 3.35 | 13.5 | 0.0 | 12.4 | 9923 | 212.0 | 1500/1500 | 1547 | 1500 | 1 | ANGLE+NAV LAUNCH | 13.4 |
| 32.00 | 26.76 | 2.03 | 14.2 | 0.0 | 6.0 | 9899 | 211.6 | 1500/1500 | 1537 | 1500 | 1 | ANGLE+NAV LAUNCH | 7.3 |
| 33.00 | 28.08 | 0.63 | 15.3 | 0.0 | -0.0 | 9895 | 211.4 | 1500/1500 | 1534 | 1500 | 1 | ANGLE | 2.3 |
| 34.00 | 28.38 | 0.15 | 16.2 | 0.0 | -1.8 | 9915 | 211.4 | 1500/1500 | 1535 | 1500 | 1 | ANGLE | 1.2 |
| 36.00 | 28.20 | -0.28 | 14.4 | 0.0 | -2.9 | 7860 | 192.5 | 1500/1500 | 1563 | 1500 | 1 | ANGLE | -2.2 |
| 38.00 | 27.92 | 0.03 | 13.8 | 0.0 | -1.4 | 7852 | 192.5 | 1500/1500 | 1575 | 1500 | 1 | ANGLE | -1.3 |
| 42.00 | 28.59 | 0.23 | 13.3 | 0.0 | -0.3 | 7844 | 192.5 | 1500/1500 | 1584 | 1500 | 1 | ANGLE | -0.4 |
| 46.00 | 29.57 | 0.24 | 13.2 | 0.0 | -0.2 | 7843 | 192.5 | 1500/1500 | 1585 | 1500 | 1 | ANGLE | -0.0 |
| 50.00 | 30.46 | 0.22 | 13.2 | 0.0 | -0.3 | 7843 | 192.5 | 1500/1500 | 1586 | 1500 | 1 | ANGLE | -0.0 |
| 54.00 | 31.34 | 0.22 | 13.2 | 0.0 | -0.3 | 7843 | 192.5 | 1500/1500 | 1585 | 1500 | 1 | ANGLE | -0.0 |


## Current baseline: preset + sensors (merge of `sensors-machine`, 2026-09-29)
The trainer bench now wires the GPS module (u-blox on uart3 + QMC5883L on I2C1) and a 4S battery, and the first
start runs `calibrate_mag` after `calibrate_acc`: iNav's estimator and launch control use the GPS, the compass
and the baro, so the flight's hash changes. **Current trainer plane-launch baseline, reason preset + sensors: Linux
`51d4a9212465`, Windows core `c66d93c7274e`** (2 runs each, 5400 rows; `../sensors/linux-2026-09-29/`,
`../sensors/windows-2026-09-29/`).

| trainer plane-launch | Linux (homelab) | Windows core |
|---|---|---|
| phase 3 script | `1e4b3b911e52` | – |
| sensors on the old script (branch only) | `af4b1a8b88f4` | – |
| preset (main `f4d83a5`) | `4f8c7114358a` | `ade0080623bb` |
| **preset + sensors (current, main `ab6d427`)** | **`51d4a9212465`** | **`c66d93c7274e`** |
| RS7 liftoff (unchanged by both) | `c5a2b9bb6c49` | `6d8d378c099f` |

The Windows core gives other hashes than Linux with the same machine code (checked with the main-built Windows
core: RS7 and the preset-only trainer), deterministic on each platform; compare hashes per platform (why:
`../determinism/README.md`). The flight is equivalent to the preset-only one and between the platforms:

| preset + sensors | Linux `51d4a9212465` | Windows `c66d93c7274e` |
|---|---|---|
| motor start | 24.17 s | 24.18 s |
| climb plateau FDM pitch / climb | 14.3° / 3.83 m/s at 13.9 | 14.3° / 3.82 m/s at 13.9 |
| iNav ends the launch | 32.28 s | 32.38 s |
| level 37..54 s | pitch −0.7 ± 0.5°, 13.0 m/s, +0.10 | pitch −0.7 ± 0.5°, 13.0 m/s, +0.10 |
| first start (empty cfg-cache) | magzero −2 1 0, maggain 1497 1508 1499, applied 72.889 s | the same |

## The configurator's airplane preset (branch `inav-defaults`, new baseline `4f8c7114358a`)
iNav Configurator 9.1.1 on the bench showed the "Default values" dialog, the red "Mixer not configured" and
"Enable motor and servo output" off, with the model configured and flying. `models/trainer/inav.cli` now writes
what the configurator's "Airplane with a Tail" leaves on a board (`models/trainer/README.md` "Configuration":
the conditions with references, the settings and what they change). `plane_evidence.py D` replays the connect and
the tabs' loads (`configurator_handshake.py --tabs`) and prints the configurator's own conditions:

| | `applied_defaults` → dialog | `appliedMixerPreset` → "Mixer not configured" | `PWM_OUTPUT_ENABLE` → outputs switch | servo rules / rates |
|---|---|---|---|---|
| old script (origin/main `e450ac1`) | 0 → **shown** | -1 → **shown** | off | roll +100/-100, pitch -100, yaw -100; rates 100 |
| new script | 3 → not shown | 14 → not shown | on | all +100 (preset 14); servo 2/3/4 rate -100 |

First start from an empty `cfg-cache` (stage C): 132 script lines at 8.12..13.25 s, `save`, ACC calibration,
acczero 41/41/41, accgain 4076/4076/4076, `config applied` at 46.04 s; `diff all` in
`diff-first-start-preset-2026-09-29.diff.txt` (the firmware adds `gyro_adaptive_filter_min_hz = 20`: `fc/config.c:196`
keeps it 5 Hz under the preset's 25 Hz gyro LPF, as it would for a configurator user).

**plane-launch** (stage L, 2 runs, **`4f8c7114358a` both**, 5400 rows; 80 host s for 54 virtual s): the new
baseline for the trainer, changed because the script changed (PIDs, rates, filters, looptime, launch motor delay,
motor protocol; RS7 unaffected: `c5a2b9bb6c49` ×2, `speed_evidence.sh "S E"`). Against `1e4b3b911e52`:

| | before | now |
|---|---|---|
| detection (motor start − `nav_fw_launch_motor_delay`) | ramp + 70 ms | ramp + 70 ms |
| motor start | 24.57 s (delay 500 ms, 0.07 s motor-off glide) | 24.17 s (delay 100 ms, still on the ramp) |
| ESC pulse at 1700 | 212.5 µs ONESHOT125 | 1700 µs PWM (STANDARD) |
| climb plateau, FDM pitch / climb | 16.8° / 4.30 m/s at 13.7 m/s | 14.3° / 3.87 m/s at 14.0 m/s (target 18°) |
| iNav ends the launch | 32.77 s | 32.37 s |
| height at 34 s | 28.4 m | 25.8 m |
| level flight 37..54 s (ANGLE, throttle 1550) | pitch −0.4 ± 0.4°, 13.3 m/s, +0.20 m/s | pitch −0.6 ± 0.4°, 13.0 m/s, +0.12 m/s |

The climb holds less pitch than the 18° target with the preset's PIFF (15/5/5/80 pitch, TPA 80, 90 °/s pitch rate)
on this airframe; not tuned here. Suites: devices, fdm, `core/qemu/tests test`, `test_wiring.py` (56), py_compile,
`equiv` 230/230, `equiv-host` 230/230.

## Ground take-off and the link lost (stage G, `flight.py --plan ground-takeoff`, `env-scenario=ground-takeoff`)
Plan: 20.0 ANGLE, 21.0 ARM, 22.0 full throttle, 24.5 pitch stick 1350 (rotate), 29.0 sticks centred +
throttle 1650, **34.0 `link:false`** (the `crsf.link_lost` fault) to 38.0, run to 50 s.

- Full-throttle roll from 22.0 s; **lift-off at 22.97 s at 11.3 m/s** (FDM analytic lift-off 11.27 m/s at zero
  pitch); after the rotation it climbs at ~2 m/s at 23.5 m/s (FDM pitch 1.2–1.6°, iNav's estimate 4.7–6.2° nose
  up: the estimator without GPS/pitot under sustained full-thrust acceleration; not ours to correct); 14.8 m max.
- **Failsafe**: the link drops at 34.0; iNav's `FAILSAFE` box at **34.84 s**, motor to 125 µs (off),
  `failsafe_procedure` LAND (default) without GPS = its emergency landing: `failsafe_fw_roll_angle` −20° bank,
  nose down; a left descending turn at 12 m/s, −2.1..−2.9 m/s, heading 0 → −92°; touchdown at 42.0 s
  (roll −19.8°, sink 2.15 m/s: under the FDM's crash thresholds, no `crash` event). The link comes back at
  38.0 (`fault` off) but iNav stays in failsafe to the end: leaving it needs stick movement beyond
  `failsafe_stick_threshold` (50) and the plan keeps the sticks centred. Events: `fault link_lost on`,
  `failsafe {"on":true,"rxloss":false}` (iNav, armed: shown by the FAILSAFE box), `fault ... off`.
- **With the configurator's airplane preset** (branch `inav-defaults` review, same plan, `e52958844ec2` ×2, not
  kept as a file; the table below is the earlier script): lift-off unchanged at 22.97 s, climbs to 22.5 m;
  `FAILSAFE` at 34.85 s, motor off (1000 µs, STANDARD). The emergency landing's yaw command
  (`failsafe_fw_yaw_rate` −45 °/s) now holds the rudder on its end stop, 1000 µs (was 1411 µs; the preset's
  `fw_ff_yaw` 255, was 60): a tighter left turn (heading 0 → −217° by 44.5 s, was −92° by 42 s), pitch −18..−20° (was
  −11..−13°), sink 2.9 m/s, and the touchdown at 44.2 s trips the FDM's crash thresholds: event `crash`
  `{"roll_deg":-17,"pitch_deg":-19.2,"sink_mps":2.85}`. Not tuned here (as the launch climb above).

| t s | height m | climb m/s | airspeed m/s | roll ° | pitch ° | motor rpm | motor µs | ail L/R µs | elev µs | rud µs | armed | mode | iNav pitch ° (nose up) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 22.00 | -0.00 | -0.00 | 0.0 | 0.0 | 0.0 | 0 | 125.0 | 1500/1500 | 1500 | 1500 | 1 | ANGLE | -0.0 |
| 22.50 | -0.00 | -0.00 | 6.5 | 0.0 | 0.0 | 13272 | 249.9 | 1500/1500 | 1500 | 1500 | 1 | ANGLE | 0.1 |
| 22.97 | 0.00 | 0.00 | 11.3 | 0.0 | -0.0 | 13456 | 249.9 | 1500/1500 | 1498 | 1500 | 1 | ANGLE | 0.4 |
| 23.50 | 0.05 | 0.16 | 16.2 | 0.0 | -2.2 | 13640 | 249.9 | 1500/1500 | 1507 | 1500 | 1 | ANGLE | -0.7 |
| 24.50 | 0.43 | 0.53 | 21.1 | 0.0 | -1.9 | 13837 | 249.9 | 1500/1500 | 1492 | 1500 | 1 | ANGLE | 1.8 |
| 25.50 | 1.66 | 1.96 | 22.9 | 0.0 | 1.5 | 13910 | 249.9 | 1500/1500 | 1490 | 1500 | 1 | ANGLE | 6.2 |
| 27.00 | 4.68 | 1.93 | 23.5 | 0.0 | 1.2 | 13935 | 249.9 | 1500/1500 | 1485 | 1500 | 1 | ANGLE | 5.5 |
| 29.00 | 8.65 | 2.13 | 23.6 | 0.0 | 1.6 | 13941 | 249.9 | 1500/1500 | 1485 | 1500 | 1 | ANGLE | 4.7 |
| 31.00 | 10.31 | 0.60 | 17.4 | 0.0 | -0.6 | 9300 | 205.3 | 1500/1500 | 1528 | 1500 | 1 | ANGLE | -4.1 |
| 34.00 | 13.37 | 1.25 | 15.4 | 0.0 | 2.6 | 9261 | 205.3 | 1500/1500 | 1547 | 1500 | 1 | ANGLE | -1.2 |
| 34.84 | 14.43 | 1.25 | 15.1 | -0.0 | 2.7 | 3873 | 125.0 | 1435/1565 | 1515 | 1407 | 1 | ANGLE+FAILSAFE | -0.8 |
| 35.50 | 14.78 | -0.51 | 13.0 | -16.3 | -5.0 | 0 | 125.0 | 1476/1523 | 1531 | 1411 | 1 | ANGLE+FAILSAFE | -8.4 |
| 36.50 | 13.00 | -2.62 | 12.1 | -18.3 | -13.7 | 0 | 125.0 | 1486/1514 | 1604 | 1411 | 1 | ANGLE+FAILSAFE | -17.4 |
| 38.00 | 8.83 | -2.60 | 12.1 | -19.3 | -12.9 | 0 | 125.0 | 1483/1517 | 1636 | 1411 | 1 | ANGLE+FAILSAFE | -16.3 |
| 40.00 | 4.32 | -2.10 | 11.5 | -20.1 | -10.7 | 0 | 125.0 | 1484/1516 | 1644 | 1411 | 1 | ANGLE+FAILSAFE | -13.3 |
| 42.00 | 0.06 | -2.15 | 11.2 | -19.8 | -10.9 | 0 | 125.0 | 1484/1516 | 1658 | 1411 | 1 | ANGLE+FAILSAFE | -12.9 |
| 42.50 | -0.00 | -0.00 | 9.1 | -0.2 | -0.1 | 0 | 125.0 | 1362/1638 | 1577 | 1410 | 1 | ANGLE+FAILSAFE | -2.8 |
| 46.00 | -0.00 | -0.00 | 2.4 | -0.0 | -0.0 | 0 | 125.0 | 1364/1636 | 1527 | 1410 | 1 | ANGLE+FAILSAFE | -6.7 |
| 50.00 | -0.00 | -0.00 | 0.7 | -0.0 | -0.0 | 0 | 125.0 | 1365/1635 | 1435 | 1410 | 1 | ANGLE+FAILSAFE | -4.7 |


## Speed and profile (stage P: `prof=on`, plane-launch to 54 s)
- The flight (virtual 24..54 s): **8.41 host s per 10 virtual s = 1.19× realtime** (prof on; the RS7 liftoff,
  prof off: 9.5–9.8 on this host). Whole run with the boot: 16.1 host s per 10 virtual s. The phase-1 boot figures (0.20–0.25×) are
  for a blank config sector; from the snapshot the boot is faster, the first-start boot (script + calibration)
  runs ≈ 0.11× until iNav's scheduler is up.
- Guest execution (TCG) **76.4 %** of wall. Top accounts (of wall): `armv7m.nvic` 5.5 % (31 M reads, 575 k per
  virtual s, 154 ns each), `vcpu.bql_wait` 5.4 %, `armv7m.systick` 4.0 % (32 M, 590 k/s): iNav's `micros()`
  (SysTick VAL + ICSR) as in phase 1; `vcpu.between_slices` 4.0 %, `main.bql_wait` 3.2 %, **`USART1` 2.9 %
  (2.6 M accesses, 48.6 k per virtual s, 951 ns each: the CRSF receiver's UART — the F2xx USART model's
  register reads by iNav's serial driver; the most expensive MMIO per access here, a phase-4 candidate)**,
  DWT 1.4 % (50 M reads at 25 ns), USART6 0.7 %, `fdm.tick` 0.62 % (the plane FDM step 0.19 %), GPIO 0.45 %,
  `ctl.tick` 0.39 %. Idle skip: 1.08 M skips. No one-line fix: nothing optimised.

## RS7 regression and tests
- RS7 liftoff (`speed_evidence.sh "S E"`, nexus-xr, the same build): FDM log **`c5a2b9bb6c49`**, 2251 rows, 2/2
  runs, after each commit touching shared code (machine FDM selection; calibration/cfg-cache/IMU error).
- `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test` (test_i2c 68, test_prof,
  test_session 202), `equiv` 212/212 through the QEMU build and `equiv-host` 212/212, `test_wiring.py` OK,
  py_compile OK.

## Caveats / not done
- GPS (`ublox` on USART3) and the SPL06 on I2C1: phase 4 (the I2C1 v1 controller stays the NACKing stub; iNav
  runs with BARO=NONE, no altitude estimate from a baro).
- The launch-detection time is inferred from the motor start (no MSP flag); `nav_fw_launch_*` are iNav's
  defaults, unchanged.
- The trainer's aerodynamics in these runs were a placeholder coefficient set, not a measured aircraft; the current
  model and its parameters are `docs/fdm.md` §3 and `models/trainer/README.md`.
- iNav's attitude estimate differs from the FDM by up to ~4.7° in the full-throttle ground climb (no GPS/pitot);
  recorded, not corrected.
- The first start of a new firmware/script/calibration costs ~4 host minutes (boot on a blank sector at ~0.11×
  plus 28 virtual s of poses); later starts restore the snapshot (`cfg-cache`).

## What the app needs (not done here: the Godot app is a separate task)
- **Preset** `presets/inav-trainer-launch.json` (app form; `aircraft: trainer`, board
  `speedybee-f405-wing`, the wiring with `output1`..`output6` pads, IMU props `accel_offset_mg` /
  `accel_scale_pct`). `Bench.ToSession` needs no change: a board without `*.config` gets no
  `board_config`, the resolver takes the base script from `board.json`. The first start of a new
  firmware/script runs the calibration (~40 virtual s, ~4 host min at the boot's 0.11×): show the
  `config` events (`applying` → `calibrating` → `applied` `acc_calibrated:true`) and the
  `config: ACC calibration: ...` log lines as progress; later starts restore the snapshot from
  `cfg-cache` (`restored`).
- **Launch button**: `{"type":"launch"}` on the control socket (defaults: the running scenario's
  `launch` object, 50 km/h, 0.5 s, 10°; optional `speed_kmh`, `duration_s`, `pitch_deg`,
  `heading_deg`); an `error` when out of range; a `log` warning when a < 22.4 m/s². Events
  `launch {phase:"start", a_mss, speed_mps, heading_deg, pitch_deg}` and
  `{phase:"release", speed_mps, height_m}`. The default scenario waits for it (the model sits 10°
  nose up on the pad from t = 1 s). The pilot needs, before throwing: ANGLE (ch6) and NAV LAUNCH
  (ch7) switches on, then arm (ch5) with the throttle low, then throttle up (iNav waits in launch
  mode, motor idle).
- **State**: `state.plane` `{airspeed_mps, alpha_deg, beta_deg, thrust_n, surfaces_deg{aileron_l,
  aileron_r, elevator, rudder}, launch: none|align|ramp, held, posed, scenario, wind_ned}`;
  `rpm` is the prop's; `actuators` by name as for the heli; signals `fdm.airspeed_mps`,
  `fdm.alpha_deg`, `fdm.beta_deg`. `fc.mode` carries iNav's boxes (`ANGLE`, `NAV LAUNCH`,
  `FAILSAFE`, `MANUAL`...), `fc.arming_flags` iNav's names (`CAL`, `ACC`, `THR`, `ARMSW`, `RX`...).
- **Visual** (`models/trainer/README.md`): fuselage/wing/tail placeholders moved by
  `pos_ned`/`quat`; surfaces rotated by `state.plane.surfaces_deg` (+ = aileron trailing edge down,
  elevator trailing edge up, rudder trailing edge right).
- **Events** to show: `launch`, `pose` (constraint holds), scenario step events + `scenario_end`,
  `mix_warning {left, right}` (both ailerons deflected the same way: a mixer/`reversed` mistake),
  `crash {roll_deg, pitch_deg, sink_mps}`, `failsafe`.
- **Environment**: `{"type":"env","scenario":"ground-takeoff"}` / `"launch"` / `null`,
  `wind_ned`, `pressure_pa` + `temp_c`; a constraint panel could send
  `{"type":"constraint","mode":"hold","euler_deg":[r,p,y|null],"rotate_s":1}` / `"free"`.
- **Channel map** (iNav AETR): 1 roll, 2 pitch, 3 throttle, 4 yaw, 5 ARM, 6 ANGLE, 7 NAV LAUNCH
  (`models/trainer/inav.cli` `aux`).

## The iNav Configurator on the MSP port (stage M, branch `fix-msp-inav`)
Report: the iNav Configurator (TCP, the address from the app's "Copy configurator address") said "Failed to open
MSP connection"; the RotorFlight configurator works on the RS7 bench. That message is iNav's `serialPortOpenFail`,
the connect itself failing before any byte is sent (a silent firmware gives "No configuration received within 10
seconds" instead). The app copied `tcp://127.0.0.1:31235`, the only form the RotorFlight configurator takes
(`src/js/serial.js`: `^tcp://host(:port)?$`). The iNav Configurator (port list → TCP, field "IP:Port",
`js/connection/connectionTcp.js`) splits the text on ':': host `tcp`, port `parseInt("//127.0.0.1")` = NaN; Node's
`net.connect` throws `ERR_SOCKET_BAD_PORT` synchronously → `callback(false)` → the message. It needs `127.0.0.1:31235`.
The app now copies (and shows) the form of the firmware's configurator: `host:port` for iNav (the core's
`hello.firmware`, before a hello the bench's firmware file), `tcp://host:port` otherwise (`app/scripts/ConfiguratorAddress.cs`,
self-test `cfg_address`).

The core's MSP path was checked byte by byte with `scripts/configurator_handshake.py` (iNav Configurator 9: MSP v2 from
the first request, `MSP.protocolVersion = PROTOCOL_V2` in `serial_backend.js`; the chain API_VERSION, FC_VARIANT,
FC_VERSION, NAME, BOXIDS, DATAFLASH_SUMMARY) while the control server polls `MSP2_INAV_STATUS`/ATTITUDE/RC on the same
USART6: every reply reaches the client whole with a good CRC8, none swallowed, no stray bytes, ~60–90 ms for the chain
at t > 17 s. From an empty cfg-cache with a client connecting every ~1.5 host s through the whole first start: no reply
only while the firmware boots (t 0–7.9, 10.3–15.0 after the script's save, 42.3–46.8 after the calibration's save),
all six replies in every other attempt, including during the script and the calibration (host bytes held in the socket
while lines/save go out, then delivered). The mute of the MSP host socket during the script ends with it (`CFG_DONE`).
RS7 (`rs7.json`, same ports): RotorFlight form connects (MSP v1, 81 ms), the iNav form too.

Two things seen on the way, not the report's cause:
- **After CLI `exit` iNav reboots and is silent ~3.6 virtual s** (the RS7's RotorFlight: 1.6 s), and the first-start /
  boot phase runs at ~0.11× on the homelab (after the boot ~1.2×): a connect in the ~33 host s after `exit` fails
  with "No configuration received within 10 seconds"; the next succeeds (log: attempt 4, 33 host s after `exit`). On
  the RS7 the connect right after `exit` succeeds within the 10 s window (6.1 host s).
- **A client polling MSP delays the core's CLI entry**: iNav acts on a pending `#` only after 100 ms without a byte on
  the port (`msp/msp_serial.c:436` `mspProcessPendingRequest`). With a client connecting every ~1.5 host s during the
  first start the script's CLI entry timed out twice (1 s each) and got in at the third try; the script itself was
  unaffected (same 13 lines, saved).

## The configurator through a flight (stage E, branch `bench-state`, 2026-09-30)
The product owner's "the configurator dropped after a while (maybe on arm/flight)". `plane_evidence.py E`
(ports 31270-31274, QEMU `~/flightbench-spike/qemu-bs`): the replayed iNav Configurator 9.1.1
(`configurator_handshake.py --tabs --poll`: connect, the tabs' reads, then its Setup tab + status bar
polling — ATTITUDE 20 Hz, RAW_GPS 4 Hz, SENSOR_STATUS/INAV_STATUS/ACTIVEBOXES/INAV_ANALOG 3.3 Hz, one
request in flight, 3 s timeout) while the core flies plane-launch to t = 60 s with the app's client
subscribed to the modes. What makes the real configurator disconnect (read in its source): only its
TCP socket ending/failing (`js/main/tcp.js` `end`/`error` → `connectionTcp.js` `abort()`) or a
(re)connect without the identification within **10 host s** (`serial_backend.js` `'connecting'`,
"No configuration received within 10 seconds"); MSP timeouts do not, and the status bar's dark link
icon (no INAV_ANALOG reply for 0.9 s) is only an icon. Node's `net.connect` resolves at once, so the
10 s run from the click, whatever the TCP accept does.

| run | result |
|---|---|
| E, no reboot | connected throughout: 2294 requests, 2294 replies, 0 timeouts, 0 replies to another request, 0 bad checksums through arming (21.1 s), the launch (24.0 s) and the flight to 60 s; latency p50 20 ms, p99 218 ms, max 353 ms. The link icon went dark 8× at t = 4.0–4.9 s only (right after the boot; see below) — **PASS** |
| `E_ARGS=--reboot=30` (armed at t = 22.6) | MSP_SET_REBOOT refused by iNav while armed (`$X!` 0x44), the configurator's close + reconnect 5 s later identifies at once and polls to 60 s: 1345/1345, p99 47 ms — **PASS** |
| `E_ARGS=--reboot=15` (disarmed at t = 9.9) | iNav reboots; the reconnect sends API_VERSION 4× in its 10 host s, t 10.46 → 11.58: **1.1 virtual s** passed, the firmware was still booting → "No configuration received within 10 seconds" — **FAIL, the drop-out** |

**Root cause of the drop-out: the boot's pace.** iNav answers MSP 3.6 virtual s after a reset
(power-on, `reset firmware` and a reboot alike: `fc.msp` turns true at t = 3.76 / 8.16 after a reset
at 4.4), and the core runs that stretch at **0.09–0.12× realtime** on this host (measured every
virtual 0.2 s through a power-on and a `reset firmware`; the host had free cores), i.e. **~33 host s**
— the same 33 s stage M measured after a CLI `exit` (above). Any connect or the configurator's
automatic reconnect inside that window fails: after a tab's "Save and Reboot", the CLI tab's `exit`,
the core's own `diff` (`exit` reboots), a `config` reset, or a connect right after the core starts.
In flight nothing drops (the core's MSP poll, the host socket handoff and the modes re-reads cost
the client no reply: 0 replies to another request in ~3900). The link icon's dark spells at
t = 4.0–4.9 are the tail of the same slow stretch (0.09× until ≈ 5 s after the boot). Not fixed
here: it is the emulation's speed during iNav's boot (busy-wait delays and the sensor probes; the
SysTick idle skip catches a fraction, `evidence/f405/`); for the 10 s window the boot needs ≥ ~0.5×.
Until then the app should show "firmware booting" (`state.fc.msp` false) and tell the user to
connect the configurator once it is up (and to press Connect again after a Save and Reboot).

**Found on the way (the harness, fixed):** the MSP port serves **one** TCP client (a QEMU socket
chardev); a second connect completes in the kernel's backlog and hangs silently until the first
closes, then gets whatever the queued client sent. `flight.py` opened an idle MSP connection even with
`--quiet` (plane plans): the replayed configurator's reconnect queued behind it (`ss`: the idle ESTAB
+ a CLOSE-WAIT with 10 unread bytes) — the first armed run "failed" for that reason. `flight.py
--quiet` no longer connects. The same can happen to a user with two tools on the port (a second
configurator window, a stuck earlier connection): the second one sees a connect and no reply.
Rejecting or taking over a second connection is a core change to discuss, not made here.


# Phase 4a: the F405's sensors under stock iNav 9.1.0 (2026-09-29)

Branch `sensors-machine`, homelab (i7-4790), QEMU 11.1.1 + this tree in `~/flightbench-spike/qemu-4a`
(`core/qemu/apply.sh`; the machine code of `d6b2f17`, the commits after it touch docs and the evidence
script only), firmware the release hex `firmware/speedybee-f405-wing/inav_9.1.0_SPEEDYBEEF405WING.hex`
(unmodified, fw hash `3bfe4bef168e`), bench `core/qemu/sessions/trainer.json` (the GPS module: `ublox` on
uart3 + `qmc5883l` on i2c1; `battery` on batt1; the board's SPL06 on i2c1) with the ports moved to
31285/31286 (HMP 31287, motion 31288, pilot 31289). Driver: `core/qemu/scripts/sensors_evidence.py "C S L
R P"`, one run, starting with an empty `cfg-cache`.

| file | what |
|---|---|
| `sensors-evidence-2026-09-29.log` | stages C (first start), S (at rest: GPS, heading, baro, battery, faults), L (plane-launch ×2 + report), R (RS7 ×2), P (prof) |
| `plane-launch-2026-09-29-run1.csv` | the plane-launch FDM log (every 10 ms, to 54 s; run 2 byte-identical), with the new columns `batt_v, batt_a, batt_mah, fc_gps_fix, fc_gps_sats, fc_lat, fc_lon, fc_baro_alt` |
| `plane-launch-prof-2026-09-29.json`, `.txt` | `prof=on` profile of the plane-launch flight, `prof.py show` |
| `tests-2026-09-29.log` | every test suite |

## What iNav's `status` shows (the trainer bench)
```
Voltage: 16.78V (4S battery - OK)
CPU Clock=168MHz, GYRO=ICM42605, ACC=ICM42605, BARO=SPL06, MAG=QMC5883
Sensor status: GYRO=OK, ACC=OK, MAG=OK, BARO=OK, RANGEFINDER=NONE, OPFLOW=NONE, PITOT=NONE, GPS=OK
I2C Errors: 0, config size: 13523, max available config: 131072
ADC channel usage:  BATTERY : configured = ADC 1, used = ADC 1 / CURRENT : configured = ADC 2, used = ADC 2
System load: 31, cycle time: 501, PID rate: 1996, RX rate: 249, System rate: 10
GPS: HW Version: UBLOX10 Proto: 34.09 Baud: 115200   SATS: 16  HDOP: 1.200  EPH: 1.000 m  EPV: 1.600 m
     GPS 1/1, Galileo 1/1, BeiDou 1/1, Glonass 0/0, Max concurrent: 4
```
(phase 1, no sensors: `System load: 12`; `BARO=NONE`, `MAG=NONE`, `0S battery - NOT PRESENT`.) The diff after the
autodetect (`baro_hardware`/`mag_hardware` stay AUTO in the script; iNav saves what it found):
`feature GPS`, `set align_mag = CW270FLIP`, `set mag_hardware = QMC5883`, `set baro_hardware = SPL06`,
`set gps_ublox_nav_hz = 10`; `get mag`: `magzero 2 / 1 / -1`, `maggain 1497 / 1499 / 1503`,
`inav_auto_mag_decl = ON`, `mag_calibration_time = 30`.

## Baselines after the merge with main (2026-09-29): which hash is current
This README is phase 4a on its branch, **before** `main`'s configurator preset (`inav-defaults`): its trainer
hash `af4b1a8b88f4` is the branch's script (the old `inav.cli`) + the sensors and is **superseded**. After the
merge (`merge-sensors`: the preset script + the sensors) the trainer's plane-launch is **`c66d93c7274e`**
(Windows core, 2 runs), the reason: preset + sensors. It was measured on Windows only (homelab unavailable);
the Windows core has its own hashes (RS7 `6d8d378c099f` on Windows vs `c5a2b9bb6c49` on Linux, before and
after the merge alike). **Linux (homelab, main `ab6d427`): `51d4a9212465`** ×2 from an empty cfg-cache, RS7
`c5a2b9bb6c49` ×2 (`linux-2026-09-29/`). `windows-2026-09-29/README.md` has the Windows run; `../plane/README.md`
the baseline table; `../determinism/README.md` why the platforms differ.

## First start (stage C): script, ACC calibration, compass calibration
Blank sector: the 15-line script at 4.27..4.58 s (the board script is comments only now; the trainer's
`inav.cli` + the GPS's lines `feature GPS`, `serial 2 +1`, `set gps_provider = UBLOX`, `set gps_ublox_nav_hz =
10`), `save`, reboot. At 9.35 s the core's poll found iNav refusing `MSP_BOXNAMES`: with the GPS and the
compass iNav has 47 mode boxes and `serializeBoxNamesReply` (`fc/fc_msp_box.c:145`) returns an error when
their names do not fit the reply; the core names the boxes from `MSP_BOXIDS` and iNav's permanent-id table
instead (before the fix its status poll waited for the names forever: `fc.msp` stayed false).
`calibrate_acc`: the six poses 11.4..31.9 s (each collected in 0.51..0.53 s), acczero 41 41 41, accgain
4076 4076 4075 (as in phase 3). `calibrate_mag` (new, `auto_first_start`): `MSP_MAG_CALIBRATION` at 36.38,
answered at 36.385; the sweep (14 hold steps computed from the compass's field and CW270FLIP mount) to 54.99;
iNav's `COMPASS` blocker cleared at 66.60 (its 30 s `mag_calibration_time`), `MSP_CALIBRATION_DATA`:
**magzero 2 1 -1, maggain 1497 1499 1503**. The sweep takes every chip axis to ±1490 LSB = |B| (49.67 µT ×
3000 LSB/G; computed from the first start's FDM attitude log), so iNav's gains (the largest reading per
axis) are |B| plus its noise peaks (σ = 2 mG = 6 LSB): equal within 0.4 %, the sphere centre within 2 LSB
of 0. Final `diff all` + `save` at 66.7; `config applied` at 67.79 with `acc_calibrated`, `mag_calibrated`.
74.5 virtual s in 220 host s. Deterministic: two first starts (this run and the development run) log
every step at the same virtual time.

**Why a calibration, and not settings:** iNav treats magzero 0/0/0 with maggain 1024/1024/1024 as
uncalibrated (`sensors/compass.c:388`: `COMPASS_CALIBRATED` off → the `COMPASS` arming blocker
(`fc/fc_core.c:274`), and `flight/imu.c:964` `isImuHeadingValid` needs a calibrated compass or a GPS heading). Our chip has no offset and one scale, so
its correct calibration is exactly the "uncalibrated" values; setting other numbers by CLI would be a
fake. The real routine is run instead, as a user would, turning the model.

## At rest (stage S, from the snapshot)
| check | result |
|---|---|
| GPS | iNav reports the GPS sensor at 4.46 s (fix 0); **3D fix at 20.06 s**, 16 sats (the receiver's `fix_delay_s` 20 from power-on; after an MCU reset the receiver keeps its fix: `SATS: 16` 6 s after the first start's reboot) |
| position | `MSP_RAW_GPS` vs the FDM position converted around home (40.1792, 44.4991), 10 samples 1 s apart on the pad: **max 0.42 m**, mean 0.25 m (the model's 0.3 m noise) |
| heading | the model held level at yaw 0, 90, 180, −90, 45, 0 (8 s still each): `MSP_ATTITUDE` heading **0, 90, 179, 270, 45, 0** — within 1° (iNav sends integer degrees, truncated). iNav's auto declination for Yerevan (6.52°, its table) = the field's |
| baro | iNav's altitude 0 m on the pad (it re-zeroes the baro while disarmed) |
| battery | iNav (`MSP2_INAV_ANALOG`): 4 cells, 16.79 V, 0.27 A (the model: 16.796 V, 0.30 A: the current sensor's ADC step is 40 mA) |
| `ublox.no_fix` / off | fix 0, 2 sats / fix back, 16 sats |
| `ublox.silence` / off | fix 0, 0 sats (iNav times out) / back after its autobaud + handshake, within 3 s |
| `ublox.spoof` 50 N −20 E | iNav's position 50.3 m N, −20.0 m E of home / back |
| `battery.voltage` 11.1 | iNav 11.09 V, state 2 (critical), 0 % (the pack stays 16.79 V) / 16.77 V |
| `battery.disconnect` | iNav 0 cells, 0.00 V, 0.00 A, state 3 / 4 cells 16.78 V |
| `qmc5883l.offset` x 150 mG | iNav's heading 90 → 82° (the geometry: ≈ 9° at this heading) / 88 → 89° |
| `qmc5883l.detached` | the `HWFAIL` arming blocker (compass unhealthy) / cleared |
| `spl06.offset` +120 Pa | 0 m: iNav re-zeroes its baro while disarmed, so a step is not visible on the pad (in flight the baro is below) |
Every change is a `fault` event; `state.faults` lists the active ones.

## plane-launch (stage L): the flight with GPS, compass and baro
Plan as phase 3 (`flight.py --plan plane-launch`, T0 20 s). Two runs: **FDM log `af4b1a8b88f4` both, 5400
rows, byte-identical** — the new baseline (phase 3: `1e4b3b911e52`). Why it changed: the firmware now has a
baro, a compass and a GPS fix, which changes its attitude/position estimation and its launch control (below);
and the log has 8 new columns (battery, iNav's GPS and altitude).

| | phase 3 (no GPS/mag/baro) | phase 4a |
|---|---|---|
| armed, mode | 21.08, ANGLE+NAV LAUNCH | 21.08, ANGLE+NAV LAUNCH |
| ramp, release | 24.000..24.490, 13.89 m/s, 0.61 m | 24.000..24.490, 13.89 m/s, 0.61 m |
| launch detection (motor start − 0.5 s) | 24.070 (+70 ms) | 24.080 (+80 ms) |
| glide before the motor | 0.07 s, min 13.61 m/s | 0.08 s, min 13.56 m/s |
| climb plateau | 27.4..30.3 s, 16.8° FDM (iNav 17.3°), 4.30 m/s at 13.7 m/s | 29.4..30.4 s, 15.2° FDM (iNav 15.7°), 4.21 m/s at 14.3 m/s |
| height at 34 s | 28.4 m | 23.1 m |
| launch ends (iNav, mode ANGLE) | 32.77 | 32.77 |
| level flight 37..54 s | roll 0.0 ± 0.0, pitch −0.4 ± 0.4, 13.3 ± 0.2 m/s, climb 0.20 ± 0.07 | roll 0.0 ± 0.0, pitch −0.4 ± 0.4, 13.3 ± 0.2 m/s, climb 0.21 ± 0.07 |
| iNav vs FDM attitude, level | 0.0 / 0.3° | 0.0 / 0.3°; heading 0 vs 0 |

The same flight except the first 2.5 s of the climb: after the motor start the pitch stays at 4–6° until
~27 s (phase 3: 9.4° at 26 s), then rises to the plateau; the climb rate at the plateau and everything
after the launch are unchanged. The cause is iNav's, from the GPS fix: `navigation_fw_launch.c:311-323`
resets the PID I-terms while `isProbablyNotFlying()`, which applies only when the position estimate is
trusted (a GPS fix) and `isFixedWingFlying()` is false — and that needs, among others, more than 5 m above
the takeoff altitude (`navigation_fixedwing.c:773`). The log: the model passes 5 m at ~27.0 s, and the pitch
climbs from there. Without a GPS (phase 3) the check never applied. This is the firmware behaving as it does
on a real plane with GPS, so the phase-3 numbers are not reproduced there and should not be.

**Battery over the flight** (the model: 4S 4200 mAh, current = 1 × the ESC model's current + 0.3 A):

| t s | throttle | motor rpm | V | A | mAh used |
|---|---|---|---|---|---|
| 19.0 (disarmed) | 0.00 | 0 | 16.798 | 0.30 | 1.6 |
| 24.6 (motor start) | 0.26 | 1200 | 16.797 | 7.15 | 2.1 |
| 25.0 (launch throttle) | 0.70 | 9968 | 16.795 | 14.53 | 3.7 |
| 32.0 | 0.69 | 9906 | 16.759 | 13.80 | 31.1 |
| 36.0 (cruise) | 0.54 | 7861 | 16.743 | 7.15 | 42.5 |
| 54.0 | 0.54 | 7843 | 16.695 | 7.33 | 79.0 |

After the hashed window (t = 55.0) iNav's own reading, `MSP2_INAV_ANALOG`: 16.69 V, 7.30 A, 80 mAh drawn,
96 % — the model 16.692 V, 7.33 A, 81.1 mAh. The ESC model's current is its electrical estimate:
`idle + max_current × throttle² × (0.3 + 0.7 × load)` with the prop's load from the FDM
(`devices/src/esc.c`), i.e. it grows with the motor's work; `current_scale` scales it.

**GPS in flight:** iNav's 3D fix from 20.06 s. `MSP_RAW_GPS` against the FDM position at the same row: mean
1.49 m, max 2.97 m (at 13 m/s the solution is up to one 10 Hz epoch + one 10 Hz MSP poll old); against the
FDM position 0.10 s earlier: mean 0.46 m, max 1.37 m. At t = 55.0: 425.6 m N / 0.6 m E vs the FDM's
426.2 N / 0.0 E. **Baro:** iNav's altitude (`MSP_ALTITUDE`, baro + GPS) vs the FDM height in flight: mean
−0.13 m, max 0.91 m. **Heading** in level flight: iNav − FDM 0° throughout.

## RS7 (stage R)
RS7 liftoff (`sessions/rs7.json`, nexus-xr, the same build), FDM log every 20 ms to 45 s: **`c5a2b9bb6c49`
twice** — the baseline holds (the nexus-xr's ADC stays the stub, no battery wired; its MSP polls are
unchanged: `MSP_RAW_GPS` is polled only while iNav reports a GPS). 9.87 / 9.69 host s per 10 virtual s
(phase 3: 9.5–9.8).

## Speed (stage P, prof=on)
Flight (virtual 24..54 s): **12.54 host s per 10 virtual s** (phase 3, the same plan without the sensors:
8.41); whole run with the boot 19.62 (16.13). `prof.py diff` against phase 3's profile, per virtual second:
- guest execution +231 ms (1233 → 1464 ms): the firmware does more — the GPS parser and the navigation
  estimator, the compass and baro tasks (iNav's `System load` 12 → 31) and its I2C transfers, which it runs
  as busy-wait loops (`i2cWaitForCompletion`);
- more vCPU ↔ main-loop hand-offs (slices 42.9k → 63.2k per virtual s; `vcpu.bql_wait` + `main.bql_wait`
  +77 ms, `between_slices` +31 ms): the new timers — `gps.tick` 2.7k deadlines per virtual s (one per byte
  at 115200 on the wire), `i2c1.timer` 1k — and the timer re-arms they bring;
- `USART3` (the GPS port) 19.4k accesses per virtual s at 1.0 µs each = 1.0 % of wall: QEMU's F2xx USART
  costs about as much per access as USART1 (phase 3's note);
- `i2c1` itself is cheap: 112k accesses per virtual s at 15 ns (0.09 % of wall); `spl06.conversion` 58
  deadlines per virtual s.
Not optimised here. Candidates: GPS bytes in a burst on the FC's RX interrupt (as the CRSF receiver's
RXFRQ path) instead of one deadline per byte; the USART model's per-access cost.

## Tests (`tests-2026-09-29.log`)
`make -C devices test` (new: `test_qmc5883l` — the driver's detect/init/read sequences, the rotation at
heading 0/90/180/270, every mount against iNav's alignment code, the faults; `test_battery`; `test_faults`
with the new catalog entries), `make -C fdm test`, `make -C core/qemu/tests test` (`test_i2c` 68,
**`test_i2c_v1` 74** — iNav's F4 I2C driver ported line by line on the v1 controller, with a scripted memory
slave and the SPL06/QMC5883L models, `test_msp` 39 (the box names by id), `test_prof`, **`test_session` 216**),
`test_wiring.py` 57, the equivalence of the two bench resolvers **250/250** through the QEMU build (`make
equiv`) and the host tool (`equiv-host`), py_compile. The preset `presets/inav-trainer-launch.json` (as the
app's `Bench.ToSession` writes it) passes `session-check` on this build: `baro=on, mag=on, gps=uart3,
battery=on` and the GPS's four CLI lines.

## Caveats
- The flight's climb differs from phase 3 for the reason above (iNav's own logic with a GPS fix);
  launch, launch end and level flight are the same.
- `ublox.silence` and the other faults are checked at rest; in flight only the normal GPS.
- The baro offset fault is not visible while disarmed (iNav re-zeroes); its effect in flight was not
  exercised in this run.
- The compass field does not follow `env` `home`: the declination/inclination/intensity are the
  device's props (Yerevan by default); for another place set them (iNav takes the declination from its
  own table when it has a GPS fix, so a field that does not match the place gives a heading offset).
- The battery feeds nothing back (by decision): the ESC keeps its 14.8 V, the pack's voltage only reaches
  the FC's sensor.
- Speed: −33 % in flight against phase 3 (above); not optimised.
- The app (Godot) was not run here; the manifest now lists `qmc5883l`, `battery` (port kind `power`, a
  new pad kind the app registers on first sight) and the implemented `spl06`/`ublox`.

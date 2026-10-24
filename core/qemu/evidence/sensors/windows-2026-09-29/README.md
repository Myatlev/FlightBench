# Sensors + the configurator preset on Windows (merge `merge-sensors`, 2026-09-29)

Branch `merge-sensors` = `main` (`f4d83a5`, the trainer's configurator airplane preset) + `sensors-machine`
(phase 4a) + review fixes, run on Windows 11 under MSYS2 MINGW64: QEMU 11.1.1 + this tree built by
`core/qemu/build.sh` in a local checkout (gcc 16.2.0), mingw Python 3.14, ports 31285..31289. Drivers:
`scripts/sensors_evidence.py "C S L R P"` (one run, empty `cfg-cache` in a temp dir, not the user's
`%LOCALAPPDATA%\flightbench\config`) and `scripts/plane_evidence.py D` (`PORT_BASE=31285`).

| file | what |
|---|---|
| `sensors-evidence-2026-09-29.log` | stages C (first start), S (at rest, faults), L (plane-launch ×2 + report), R (RS7 ×2), P (speed) |
| `plane-launch-2026-09-29-run1.csv` | the plane-launch FDM log (run 2 byte-identical) |
| `configurator-tabs-2026-09-29.log` | stage D: what the iNav Configurator 9.1.1 sees after the connect |
| `tests-2026-09-29.log` | every suite, `equiv` / `equiv-host` 250/250 |

The `ConnectionResetError` tracebacks in the log are the client's reader thread when a core is stopped
(Windows resets the socket where Linux reads EOF); fixed afterwards in `tools/core_client.py`.

## iNav `status` (first start, then after the faults)
```
Voltage: 16.78V (4S battery - OK)
CPU Clock=168MHz, GYRO=ICM42605, ACC=ICM42605, BARO=SPL06, MAG=QMC5883
Sensor status: GYRO=OK, ACC=OK, MAG=OK, BARO=OK, RANGEFINDER=NONE, OPFLOW=NONE, PITOT=NONE, GPS=OK
I2C Errors: 0, config size: 13523, max available config: 131072
System load: 26, cycle time: 1004, PID rate: 996, RX rate: 250, System rate: 10   (looptime 1000: the preset)
GPS: HW Version: UBLOX10 Proto: 34.09 Baud: 115200   SATS: 16  HDOP: 1.200
```
First start: 134 script lines (the preset's 130 + the GPS's 4: `feature GPS`, `serial 2 +1`, `gps_provider`,
`gps_ublox_nav_hz`), `save`; `calibrate_acc` acczero 41 41 41, accgain 4076 4076 4075; `calibrate_mag`
sweep 41.46..60.06 s, **magzero −2 1 0, maggain 1497 1508 1499** (|B| = 1490 LSB + noise peaks); `applied` at
72.89 s with `acc_calibrated`, `mag_calibrated`; 199 host s. Saved diff: `align_mag = CW270FLIP`,
`mag_hardware = QMC5883`, `baro_hardware = SPL06`, `feature GPS`, `feature PWM_OUTPUT_ENABLE`.

## At rest (stage S)
GPS 3D fix at 20.13 s (16 sats); `MSP_RAW_GPS` vs FDM around home: max 0.44 m. Heading at yaw
0/90/180/−90/45/0: iNav 0/89/180/270/45/0 (max 1°, integer degrees). Battery: iNav 4 cells 16.79 V 0.27 A
(model 16.796 V 0.30 A). Faults: `no_fix` fix 0 / back; `silence` fix 0 sats 0 / back within 3 s; `spoof`
50.2 m N −20.0 m E / back; `battery.voltage 11.1` → iNav 11.09 V state 2; `disconnect` → 0 cells 0 V
state 3 / back; `qmc5883l.offset` heading 90 → 82°; `detached` → `HWFAIL`; `spl06.offset` invisible
while disarmed (re-zeroed).

## plane-launch (stage L), two runs: `c66d93c7274e` both (5400 rows) — Windows
| | main, preset only (Linux `4f8c7114358a`) | preset + sensors (Windows `c66d93c7274e`) |
|---|---|---|
| armed / launch mode | 21.08 | 21.08 |
| motor start | 24.17 | 24.18 |
| climb plateau FDM pitch / climb | 14.3° / 3.87 m/s at 14.0 | 14.3° / 3.82 m/s at 13.9 |
| iNav ends the launch | 32.37 | 32.38 |
| height at 34 s | 25.8 m | 26.5 m |
| level 37..54 s | pitch −0.6 ± 0.4, 13.0 m/s, +0.12 | pitch −0.7 ± 0.5, 13.0 m/s, +0.10 |

(The log's "detection = motor start − 0.5 s" line used flight_report's default delay; the script's
`nav_fw_launch_motor_delay` is 100 ms — `sensors_evidence.py` now passes it.) Battery over the flight: 16.798 V /
0.30 A disarmed → 14.37 A at launch throttle 0.70 → 6.88 A cruise at 0.525; 77.9 mAh at 54 s; iNav at 55.0 s
16.69 V 6.89 A 79 mAh 96 % (model 16.693 V 6.882 A 79.9 mAh). GPS in flight vs FDM: mean 1.46 m, max 2.97 m
(0.45 / 1.38 m against the FDM 0.1 s earlier); at 55 s 415.8 m N vs FDM 416.4 m. Baro vs FDM height:
mean −0.14 m, max 0.85 m. Heading in level flight: max 1°.

## RS7 (stage R) and the Windows/Linux hash difference
RS7 liftoff: **`6d8d378c099f` ×2** — not the Linux baseline `c5a2b9bb6c49`. The same stage with the Windows core
packaged from main before the merge (`dist-core`, `87194cc`, machine code identical to `f4d83a5`) gives
**`6d8d378c099f` ×2** too, so the merge does not change RS7; the Windows core has its own baselines. Likewise the
trainer on main (preset only) is `ade0080623bb` ×2 on Windows vs `4f8c7114358a` on Linux (same first-start
timing to the ms; the logs first differ in when the MSP-polled `fc_*` columns begin, 3.59 s, then in the physics).
The Linux hash of preset + sensors was not measured (homelab unavailable).

## Speed (stage P)
15.04 host s per 10 virtual s in flight (prof=on). `prof.py run` wrote no profile JSON here (not investigated).

## Configurator (stage D): PASS
applied_defaults 3 → dialog not shown; appliedMixerPreset 14 → banner not shown; `PWM_OUTPUT_ENABLE` on;
servo rules all +100, servos 2/3/4 rate −100.

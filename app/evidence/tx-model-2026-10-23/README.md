# tx-model — the RS7 takes off from the keyboard (2026-10-23)

Branch `tx-model` (commit `cb32647` for the build below), Windows 11, Godot 4.7.2 .NET, the packaged core
`<core dir>` (QEMU v11.1.1 + core patches, bundled by the export).

## What ran

1. **Exported build**, release profile:
   `powershell -ExecutionPolicy Bypass -File app\tools\export.ps1 -Presets 'rs7-first-flight'` → `dist\FlightBench`
   (v0.1.0, cb32647), copied to `%TEMP%\fb-txlive-run3\FlightBench`, run with an empty `APPDATA`
   (`%TEMP%\fb-txlive-run3\appdata`):
   ```
   FB_PRESET=rs7-first-flight FB_LIVE_CHECK=1 FB_SCREENSHOT=<this folder>\live.png FB_CORE_PORTS=31290 FB_NO_JOY=1 FlightBench.exe
   ```
   The preset as shipped (`presets/rs7-first-flight.json` with its `transmitter` block: ch5 `auto`, ch6 `switch2`
   `throttle`), no Settings visit. `FB_CORE_PORTS=31290` keeps the core off the default ports (the user's own
   FlightBench may hold 31250 / 31235–31238). `FB_NO_JOY=1`: this PC has an Xbox controller connected
   (`XInput Controller`); with a pad the app reads channels 1..4 from it and the keyboard's W is not read, so the
   keyboard take-off needs the pad ignored (see 3). The core the app started (pid 27724) stopped with the app; no
   `qemu-system-arm.exe` left.
2. **Self-tests** `FB_SELFTEST=all` (editor build, headless; fake core `python tools/fake_core.py --port 31260`,
   `FB_CORE_EXE=…\dist-core\qemu-system-arm.exe`, `FB_BENCH_FILE=…\rs7-win.json`, `FB_CORE_PORTS=31290`, temp `APPDATA` /
   `FB_BENCH_DIR` / `XDG_CACHE_HOME`): **45 PASS, 0 FAIL** (`main`'s 44 + `tx_model`) — `selftest-all.log`.
3. The same exported run **without** `FB_NO_JOY` (the Xbox pad read): the take-off stops at once with the reason —
   `live-check-with-gamepad.log`.

## PASS lines (`live-check.log`)

```
[live-check] PASS: Transmitter 'modes: from firmware (t = 10.19 s) · ch6 throttle: bench switch' switches [ch5 (AUX1): — | ARM; ch6 throttle (bench switch2): — | —] (variant RTFL); Functions [calibrate_acc, calibrate_gyro] from live core; Config 'diff all from the firmware (on request, t = 8.36 s, 2026-10-23 13:26), 42 lines'; Status 'connected · realtime 1.00× · 100 states/s' / 'disarmed · [ACRO] · cpu 9%' / 'OSD: none · baro -0.01 m'; Aircraft rows alt,speed,power,surfaces; configurator tcp://127.0.0.1:31291; core Core: running · pid 27724 · control 31290, msp 31291
[live-check] take-off: armed at t = 16.71 s (key '5', ch5 2000 µs)
[live-check] take-off: head 1602 rpm at t = 24.21 s (7.5 virtual s after the throttle switch, ch6 2000 µs); holding W
[live-check] PASS: take-off from the keyboard: armed at t = 16.71 s (key '5' → ch5 2000 µs), throttle on (key '6' → ch6 2000 µs), W held 1.7 virtual s (ch3 1837 µs) → altitude 0.52 m at t = 25.87 s, head 1776 rpm; Aircraft 'head 1776 rpm · motor 15850 rpm · collective +15.5° · thrust 136 N · throttle 71%'
```

## Numbers

| | |
|---|---|
| modes read (firmware, after the diff refresh's reboot) | t = 10.19 s: one range, ARM on ch5 (rows `ch5 — | ARM`, `ch6 throttle — | —`) |
| key 5 (switch_1, row 1 = ch5) | ch5 1000 → 2000 µs, `armed` event at t = 16.71 s |
| key 6 (switch_2, row 2 = ch6 `throttle`, bench switch2) | ch6 1000 → 2000 µs; head 1602 rpm 7.5 virtual s later (threshold 1600 ≈ 90 % of `gov_headspeed` 1800) |
| W held (keyboard collective, ch3) | 1.7 virtual s, ch3 1000 → 1837 µs |
| airborne | altitude 0.52 m at t = 25.87 s, head 1776 rpm, thrust 136 N |
| whole run | 2717 states, last t = 27.38 s, 0 commands dropped |
| without `FB_NO_JOY` (Xbox pad read) | `FAIL: take-off: joypad 0 'XInput Controller' drives the collective (ch3 axis 3): the keyboard's W is not read with a pad connected — unplug it or run with FB_NO_JOY=1` |

## Files

- `live.png` — the layout once the firmware answers disarmed (Transmitter: `ch5 — | ARM [5]`, `ch6 throttle — | — [6]`).
- `live-views.png` — Functions + Config windows over the layout (the views' PASS).
- `live-spool.png` — armed, throttle on, head at 1600 rpm (Transmitter: ARM and ch6 high pressed).
- `live-airborne.png` — 0.52 m up: Transmitter rows, ch3 1837 µs, Aircraft window's rotor row, Log with the key lines.
- `edit-bench-transmitter.png` — New bench → Summary → Transmitter on the nexus-xr (RotorFlight default: ch6 switch 2 `throttle`).
- `live-check.log`, `live-check-with-gamepad.log` — the app's stdout of the two exported runs.
- `selftest-all.log` — the `[selftest]` verdict lines of the full suite.

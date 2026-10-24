# kb-layer — the keyboard is a layer over the joypad (2026-10-23)

Branch `kb-layer` (code at `3926091`; the build below), Windows 11, Godot 4.7.2 .NET, the packaged core
`<core dir>` (bundled by the export). The test PC has an Xbox controller plugged in
(`XInput Controller`, joypad 0) for every run below — the condition of the 2026-10-23 bug (with a pad, W did nothing).

## What ran

1. **Exported build**, release profile: `powershell -ExecutionPolicy Bypass -File app\tools\export.ps1 -Presets 'rs7-first-flight'`
   → `dist\FlightBench` (v0.1.0, 3926091), copied to `%TEMP%\fb-kblive-<run>\FlightBench`, run with an empty `APPDATA`
   (`%TEMP%\fb-kblive-<run>\appdata`), the preset as shipped, no Settings visit, twice:
   ```
   FB_PRESET=rs7-first-flight FB_LIVE_CHECK=1 FB_CORE_PORTS=31290 FB_SCREENSHOT=<folder>\live.png FlightBench.exe            # with-joypad/
   FB_PRESET=rs7-first-flight FB_LIVE_CHECK=1 FB_CORE_PORTS=31290 FB_SCREENSHOT=<folder>\live.png FB_NO_JOY=1 FlightBench.exe  # no-joy/
   ```
   Each run's core (pid 17764, pid 13248) stopped with the app; no `qemu-system-arm.exe` left after either.
2. **Self-tests** `FB_SELFTEST=all` (editor build, headless; fake core `python tools/fake_core.py --port 31260`,
   `FB_CORE=127.0.0.1:31260`, `FB_CORE_EXE=…\dist-core\qemu-system-arm.exe`, `FB_BENCH_FILE=…\rs7-win.json`,
   `FB_CORE_PORTS=31290`, temp `APPDATA` / `FB_BENCH_DIR` / `XDG_CACHE_HOME`): **46 PASS, 0 FAIL** (`main`'s 45 +
   `keys_hint`) — `selftest-all.log`.

## PASS lines

With the Xbox pad plugged in (`with-joypad/live-check.log`):
```
[live-check] PASS: Transmitter 'modes: from firmware (t = 10.20 s) · ch6 throttle: bench switch' switches [ch5 (AUX1): — | ARM; ch6 throttle (bench switch2): — | —] (variant RTFL); Functions [calibrate_acc, calibrate_gyro] from live core; Config 'diff all from the firmware (on request, t = 8.37 s, 2026-10-23 13:59), 42 lines'; Status 'connected · realtime 1.00× · 100 states/s' / 'disarmed · [ACRO] · flags: CALIB · cpu 8%' / 'OSD: none · baro 0.00 m'; Aircraft rows alt,speed,power,surfaces; configurator tcp://127.0.0.1:31291; core Core: running · pid 17764 · control 31290, msp 31291
[live-check] PASS: take-off from the keyboard: armed at t = 16.42 s (key '5' → ch5 2000 µs), throttle on (key '6' → ch6 2000 µs), W held 0.7 virtual s (ch3 1500 → 1853 µs, held by kb) → altitude 0.56 m at t = 23.80 s, head 1669 rpm; Aircraft 'head 1669 rpm · motor 14896 rpm · collective +16.3° · thrust 135 N · throttle 67%'; joypad 0 'XInput Controller' (raw ch1=a0 0.000 ch2=a1 0.000 ch3=a3 0.000 ch4=a2 0.000; owners ch1=joy ch2=joy ch3=kb ch4=joy)
```
With `FB_NO_JOY=1` (`no-joy/live-check.log`):
```
[live-check] PASS: Transmitter 'modes: from firmware (t = 10.19 s) · ch6 throttle: bench switch' switches [ch5 (AUX1): — | ARM; ch6 throttle (bench switch2): — | —] (variant RTFL); Functions [calibrate_acc, calibrate_gyro] from live core; Config 'diff all from the firmware (on request, t = 8.36 s, 2026-10-23 14:00), 42 lines'; Status 'connected · realtime 1.00× · 100 states/s' / 'disarmed · [ACRO] · flags: CALIB · cpu 8%' / 'OSD: none · baro 0.00 m'; Aircraft rows alt,speed,power,surfaces; configurator tcp://127.0.0.1:31291; core Core: running · pid 13248 · control 31290, msp 31291
[live-check] PASS: take-off from the keyboard: armed at t = 16.41 s (key '5' → ch5 2000 µs), throttle on (key '6' → ch6 2000 µs), W held 1.7 virtual s (ch3 1000 → 1833 µs, held by kb) → altitude 0.51 m at t = 25.58 s, head 1776 rpm; Aircraft 'head 1776 rpm · motor 15850 rpm · collective +15.3° · thrust 135 N · throttle 71%'; joypad none (channels 1..4 from the keys; owners ch1=- ch2=- ch3=kb ch4=-)
```

## Numbers

| | with the pad | `FB_NO_JOY=1` |
|---|---|---|
| joypad read | 0 `XInput Controller`; raw axes at rest a0 / a1 / a2 / a3 = 0.000 (centred: no interference) | none (1 connected, ignored) |
| owners at the take-off start | ch1..4 `joy` | ch1..4 none (the key layer at rest) |
| key 5 → ch5 2000 µs, `armed` | t = 16.42 s | t = 16.41 s |
| key 6 → ch6 2000 µs, head ≥ 1600 rpm | 1603 rpm, 6.7 virtual s later | 1601 rpm, 7.5 virtual s later |
| W held: ch3 | 1500 → 1853 µs (the integrator continued from the pad's value), owner `kb` | 1000 → 1833 µs (from the key layer's low end), owner `kb` |
| airborne | 0.56 m at t = 23.80 s, 0.7 virtual s of W | 0.51 m at t = 25.58 s, 1.7 virtual s of W |
| Settings → Keys (over the bench) | `Switch 1 (ch5 ARM)`, `Switch 2 (ch6 throttle)`, `Collective / throttle up (ch3 +; integrator, holds)` = W | same |
| Settings → Transmitter | `Joypad: XInput Controller — connected` | `Joypad: — not plugged in (channels from the keys and the Transmitter window)` |
| whole run | 3061 states, last t = 30.82 s, 0 commands dropped | 3240 states, last t = 32.61 s, 0 commands dropped |

With the pad the collective sits at the pad's stick centre (1500 µs) until W takes it, so the climb starts from mid
collective and needs less W time; arming was not refused at 1500 (RotorFlight arms on ch5 with ch6 low).

## Files (per folder: `with-joypad/`, `no-joy/`)

- `live.png` — the layout once the firmware answers disarmed; `live-views.png` — Functions + Config over it.
- `live-spool.png` — armed, throttle on, head at 1600 rpm.
- `live-airborne.png` — airborne: the Transmitter window's bars with the owner marks (`1 A joy`, `3 C kb`, `5 AUX1 kb`,
  `6 AUX2 kb` with the pad; `3 C kb`, `5 AUX1 kb`, `6 AUX2 kb` without) and the hint line from the bind table.
- `live-settings-keys.png` — Settings over the bench → Keys: Sticks, Transmitter switches named after the bench's rows.
- `live-settings-transmitter.png` — Settings over the bench → Transmitter: the joypad line.
- `live-check.log` — the app's stdout.

`selftest-all.log` — the `[selftest]` verdict lines of the full suite (46 PASS, 0 FAIL).

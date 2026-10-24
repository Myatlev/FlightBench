# Does the stock RotorFlight hex support the MSP DisplayPort OSD? (2026-09-30)

Question: `core/qemu/evidence/stock-rf/README.md` validated MSP-on-USART1 for the generic stock
`rotorflight_4.6.0_STM32F7X2.hex` but left the OSD preset on the fork ELF, "whether OSD/DisplayPort
support is compiled into the generic stock `STM32F7X2` target too was not checked". This pass checks
it directly against `sessions/rs7-osd.json` (the `msp_osd` device on uart4), own core on ports
31270..31274, build tree `~/flightbench-spike/qemu-rf` (same tree as the stock-rf pass).
**Answer: no — the stock hex does not run the OSD.**

## What was run
A copy of `sessions/rs7-osd.json` with `firmware` pointed at the bundled stock hex
(`rs7-osd-stock-session.json`, ports moved to 31270/31271), booted through `run.sh --session`
(`boot-core-2026-09-30.log`: the report shows `osd=uart4` in the machine's `-M` string, same as with
the fork ELF — the device is wired regardless of firmware), then:

- `osd_evidence.py boot --port 31270 --until 12 --msp 31271 --cli` (`boot-osd-2026-09-30.log`): only
  **2 messages total** in 12 virtual s, both `clear:true` with **0 cells** (`first message: t=1.43
  clear=True rows=18 cols=50 frame=1 cells=0`); no deltas; canvas stays blank the whole window.
  Compare the fork ELF (`core/qemu/README.md` OSD stage B): first message at t=1.39, full again at
  2.92, then a steady stream of deltas (0..156 cells, 12.00 Hz), with real content (mode `ACRO`,
  timers, `DISARMED`, crosshair, rpm, battery, debug line).
- CLI `status`, `version`, `tasks`, `resource`, `diff all` (`cli-status-2026-09-30.log`):
  - `version`: `Rotorflight / STM32F7X2 (S7X2) 4.6.0 Jun 30 2026 / 07:20:47 (118e912) MSP API: 12.9`
    (the same stock binary as `stock-rf`'s).
  - `status` has **no `OSD:` line** at all (iNav's stock hex, `evidence/inav-osd/README.md`, prints
    `OSD: MSP DisplayPort [50 x 18]`; the fork ELF does too, `core/qemu/README.md` OSD stage B/E).
  - `tasks` lists `SYSTEM, GYRO, FILTER, PID, ACC, ATTITUDE, RX, SERIAL, DISPATCH, BATTERY_VOLTAGE,
    BATTERY_CURRENT, BATTERY_ALERTS, BEEPER, BARO, TELEMETRY, ADCINTERNAL` — **no `OSD` task** (the
    fork ELF's OSD task runs at 12 Hz, `core/qemu/README.md` OSD stage B).
  - `get displayport_msp`, `get vcd_video_system` still answer (`displayport_msp_serial = 3`,
    `vcd_video_system = AUTO`): the DisplayPort *settings* table is compiled in, but nothing drives it
    — the 2 blank `clear` frames look like MSP port/handshake plumbing, not the OSD render task
    (which does not exist in this build).
  - `grep -i osd` over the whole log (`status`+`version`+`tasks`+`resource`+`diff all`): **zero
    matches** beyond the `displayport_msp`/`vcd_video_system` settings lines already quoted.

Conclusion: the generic stock `STM32F7X2` target (built for the official 4.6.0 release, no `_OSD`
board target, no `USE_MSP_UART`/`USE_OSD` options) does not compile in `USE_OSD`. The fork's
`_OSD` target (`src/main/target/NEXUS_XR_OSD`, `OPTIONS="USE_MSP_UART"`) is still required for the OSD
preset. **`presets/rs7-fpv-osd.json` and `sessions/rs7-osd.json` are left on the fork ELF, unchanged
by this pass** (per the brief: stop and report if the stock target lacks DisplayPort support).

## Dev sessions repointed to the bundled files (separate from the above)
`sessions/rs7.json`, `sessions/rs7-osd.json`, `sessions/rs7-pads.json` pointed at a local dev build
path outside the repo (`~/flightbench-spike/fw-msp/obj/main/rotorflight_STM32F7X2_OSD.elf`), so a
fresh checkout without that path couldn't run them. That file is byte-identical
(sha256 `18aec709ea70...a323e`, confirmed with `sha256sum`) to the bundled
`firmware/nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf`; all three sessions now point at the
bundled copy (relative to the data root) instead. No session moved to the stock hex — the OSD finding
above means `rs7-osd.json`/`rs7-pads.json` (both wire or can wire `msp_osd`) need the fork build, and
`rs7.json` is kept identical to them on purpose (`core/qemu/README.md`'s equivalence suite diffs all
three against each other).

`rs7.json` baseline re-verified once on the repointed (but byte-identical) path,
`~/flightbench-spike/qemu-rf` build, ports 31270..31274 (`rs7-baseline-2026-09-30.log`):

```
S-this-1: c5a2b9bb6c49 2251 rows
```

Holds (`core/qemu/README.md`'s RS7 liftoff baseline, Linux).

## Files
| file | what |
|---|---|
| `rs7-osd-stock-session.json` | `sessions/rs7-osd.json` with `firmware` pointed at the stock hex, ports 31270/31271 |
| `boot-core-2026-09-30.log` | core stdout/stderr booting that session (shows `osd=uart4` wired) |
| `boot-osd-2026-09-30.log` | `osd_evidence.py boot --until 12 --cli`: message count/rate, canvas art, CLI dump |
| `cli-status-2026-09-30.log` | `status`, `version`, `tasks`, `resource`, `diff all` over the control socket |
| `rs7-baseline-2026-09-30.log` | `speed_evidence.sh "S E"` on `sessions/rs7.json` (repointed path), 1 run: hash + speed |

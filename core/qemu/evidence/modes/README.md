# Modes, diff, environment functions (2026-09-29)

Branch `core-modes`, homelab (i7-4790), QEMU 11.1.1 + this tree built in `~/flightbench-spike/qemu-prof`.
Driver `core/qemu/scripts/modes_evidence.py` (stages R = RS7 / RotorFlight 4.6.0 on the nexus-xr, T = trainer /
iNav 9.1.0 on the speedybee-f405-wing), ports 31280–31284. Protocol: `docs/protocol.md` `modes`, `diff`,
`function`, `hello.osd`, `state.fc.boxes`, the `config` replay.

| file | what |
|---|---|
| `modes-evidence-2026-09-29.log` | the driver's log: R and T, every message abridged, the core's own lines |
| `modes-rs7-2026-09-29.json`, `modes-trainer-2026-09-29.json` | the `modes` message of each firmware, whole |
| `diff-rs7-2026-09-29.diff.txt` | RS7 `diff all` on request (RotorFlight's diff is relative to its custom defaults = the bench profile, so it is short) |
| `diff-trainer-2026-09-29.diff.txt` | trainer, captured in the first start's CLI session before the final `save` (the script's `aux`, `smix`, the ACC calibration) |
| `diff-trainer-request-2026-09-29.diff.txt` | trainer, `diff` on request after a standalone `calibrate_acc` (differs from the snapshot's in the new accgain and the boot-measured `gyro_zero`/`ins_gravity_cmss`) |
| `messages-*.json` | every `modes`/`diff`/`function`/`error`/`config`/`reboot` message of each run |
| `replay-trainer-2026-09-29.json` | what a client connecting at t > 8 got after `hello` (the restored config is in the log: `replay` inside the event's data) |
| `gates-2026-09-29.log` | the regression gates below |
| `plane-launch-2026-09-29.csv` | the trainer `plane-launch` FDM log of this branch (10 ms rows to 54 s) |

## RotorFlight (RS7)
- `modes` 1.68 s after power-on: 18 boxes (ARM 0, ANGLE 1, HORIZON 2, TRAINER 47, RESCUE 53, FAILSAFE 27, ...,
  GOVERNOR BYPASS 57), one range `{"slot":0,"channel":0,"rc_channel":5,"rx_channel":4,"box":0,"start_us":1700,
  "end_us":2100,"logic":"or"}` = the profile's `aux 0 0 0 1700 2100` (RotorFlight steps: 1500 + 5 × step);
  `rx_map [0,1,3,2,5,4,6,7]` (AECR1T23), so aux 0 = firmware channel 5 = receiver channel 5 (index 4) — the ARM
  switch the app sends on `rc_us[4]`.
- Armed (ch5 high, throttle ch6 low): `state.fc.mode "ACRO"`, `state.fc.boxes [0]`; disarmed `[]`. `diff` while armed:
  `{"error":"armed","stale":true,"text":null}`; `calibrate_gyro` while armed: `error`.
- `diff` on request: CLI, `diff all` (64 lines, 0.15 s), `exit`, `reboot`, `modes` again 1.66 s later.
- `calibrate_gyro`: 2 s still, MCU reset, `CALIB` clear 6.43 s after the reset → `done`, `gyro_calibrated:true`.
- `calibrate_acc` (level): `MSP_ACC_CALIBRATION`, `CALIB` seen, cleared, `NO_ACC_CAL` not set → `done` in 0.43 s;
  RotorFlight saved it itself, so the diff is marked stale (a `diff` message with `stale:true`).
- `launch` on the heli: `error` (not in its catalog). `modes`/`diff` while the firmware is in the CLI (after `cli
  status`): `{"type":"modes","error":"cli"}`, `diff` with `error:"cli"`, `stale:true` and the last text.

## iNav (trainer)
- First start, empty cfg-cache: `applying` 7.94 → script `save` → `calibrating {"function":"calibrate_acc"}` 9.27 +
  `function start` → `modes` 12.83 (34 boxes in one reply: ARM 0, ANGLE 1, NAV LAUNCH 36, FAILSAFE 27, ...) → the six
  positions collected at 15.334 / 19.351 / 23.318 / 27.346 / 31.303 / 35.324 s (the same instants as phase 3) →
  `done` 39.79 (acczero 41 41 41, accgain 4075 4075 4076, identical to phase 3) → CLI, `diff` 39.91 (`source:
  "first_start"`, 154 lines) → `save` → `applied {"functions":["calibrate_acc"],"acc_calibrated":true}` 40.97 (phase
  3: 40.96) → `modes` again 44.53. Ranges: ARM on rc 4, ANGLE rc 5, NAV LAUNCH rc 6, 1700..2100 (900 + 25 × step);
  `rx_map [0,1,3,2]` (iNav maps 4). `hello.osd {"kind":"max7456","cols":30,"rows":16,"video_system":null,
  "video":false}`, after the firmware set VM0: `"video_system":"PAL"`.
- Second start from the snapshot, a client connecting at t = 8.8: `config {"state":"restored","replay":true}` and
  `diff` (`source:"snapshot"`, replay) right after `hello`/`manifest`/`signals`; no `modes` until it sends
  `subscribe {"modes":true}` (8.88) — then the core reads them at once (the same t) and keeps it posted after each boot.
- `launch` function: `start` → `step` (ramp) → `done` "released at 13.89 m/s, 0.60 m" in 0.5 s.
- `calibrate_gyro`: held level, MCU reset, `CAL` clear 6.66 s after → `done`.
- `calibrate_acc` standalone: six positions, CLI `save` (the snapshot stays the first start's), `done` after the
  reboot with acczero 41 41 41, accgain 4076 4076 4076.
- `diff` `refresh:false` then: the snapshot's text with `stale:true`; `diff` right after that save's reboot →
  `error:"calibrating"` (the firmware's boot calibration, `CAL` set) at 56.9 / 57.9 / 58.9 s, then a fresh one at
  60.07 (`source:"request"`, 152 lines).

## Decoder tests
`make -C core/qemu/tests test` → `test_msp: 37 checks, 0 failed`, on `core/qemu/tests/fixtures/msp-rotorflight-rs7.json`
and `msp-inav-trainer.json` (raw MSP v2 replies, `scripts/msp_capture.py`), plus synthetic RotorFlight page-2 /
AND / linked slots. `test_session` 206/206, `test_wiring.py` 56 OK, `equiv` and `equiv-host` 230/230 (18 new
`functions` cases, rejected identically by the C resolver and the Python reference), `devices`/`fdm` suites OK.
`make -C core/qemu/tests fuzz-msp` (ASan/UBSan): 43449 decoder calls on truncated/oversized/random replies, no crash.
Re-run on the review build (0365c93): all of the above pass, unchanged counts.

## Regression gates (`gates-2026-09-29.log`)
The core reads the modes by itself only while a client is subscribed (`subscribe {"modes":true}`, the app);
with no subscriber the board sees only the regular polls, and main's hashes hold. Review decision (b): the
sim does not change the board's behaviour unless asked.

| gate (2 runs each, all deterministic) | main | core-modes, no subscriber (scripts) | core-modes, subscribed from t = 0 (app) |
|---|---|---|---|
| RS7 liftoff FDM log to 45 s | `c5a2b9bb6c49` | `c5a2b9bb6c49` ×2 | `4854bd97fc60` ×2 |
| trainer `plane-launch` FDM log to 54 s (own first start from an empty cfg-cache, incl. its `diff all`) | `1e4b3b911e52` | `1e4b3b911e52` ×2 | `7e26482365cb` ×2 |

- The first start's `diff all` (in the same CLI session as the final save) does not change the trainer hash
  by itself: no-subscriber runs from this branch's own first start give main's hash.
- With a subscriber the modes are read ~1.7 s (RS7) / ~3.6 s (trainer) after each boot: 4–5 extra MSP
  requests between the regular polls. That shifts the firmware's later timing, so the log first differs at row 334 (RS7).
  The flights are equivalent (the author's run, below): RS7 lift-off 33.82 s, attitudes at 35/40/45 s, head
  speed 1793..1802, 2.310 m at 45 s, all identical to the printed precision; trainer launch detection 24.08 s
  (main 24.07), climb plateau 16.8° / 4.30 m/s, 28.4 m at 34 s, level flight as in main's table.
- App-mode baselines (subscribed from t = 0): RS7 `4854bd97fc60`, trainer `7e26482365cb`. A subscription made
  later reads at that moment instead, so the hash then depends on when it came.

Before the review (the author's runs, modes read at every boot for everyone): `4854bd97fc60` / `7e26482365cb`;
with the read disabled `c5a2b9bb6c49`, and trainer `4be8f9c8d254` from its own first start (not reproduced
with the subscription build: its own first start gives main's `1e4b3b911e52`).

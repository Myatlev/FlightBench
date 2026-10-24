# Evidence: the firmware's config as bench state (branch `bench-state`, 2026-09-30)

`scripts/bench_state_evidence.py "S M C R G X F"` on the homelab (QEMU `~/flightbench-spike/qemu-bs`),
the trainer (iNav 9.1.0, speedybee-f405-wing) on ports 31270-31274, an empty cfg-cache and one
bench-state file for the whole run. Log: `bench-state-evidence-2026-09-30.log` (commit 7bb23db); the gates: `gates-2026-09-30.log`; the metadata after
each stage: `<stage>.state.json` (the sector itself by its `sector_sha256`).

| stage | what | result |
|---|---|---|
| S | first start without the GPS: 130 script lines, `calibrate_acc` + `calibrate_mag`; the firmware's 12 saves in that run, the file written from the script's `save` on (the saves before it are the firmware's own defaults, not kept) | PASS |
| M | the Modes tab "Save" replayed as `tabs/auxiliary.js` does it: all 40 ranges by `MSP_SET_MODE_RANGE` in the UI's order (grouped by mode, so NAV LAUNCH moves from slot 2 to slot 5), then `MSP_EEPROM_WRITE`, no reboot; touching (ANGLE 900-1300 / HORIZON 1300-1700) and overlapping (MANUAL 1500-2100) ranges on aux 4. `modes` 0.45 s after the write's reply equals the firmware's `MSP_MODE_RANGES` slot by slot; the state file rewritten after the save | PASS |
| C | a host tool's `#` on the MSP port → `cli {open:true,by:"client",via:"msp"}`, `state.fc.cli` true; `{"type":"cli","exit":true}` → reboot, `cli {open:false}`, the modes still those of M; `exit` again → `error` "not in its CLI" | PASS |
| R | restart: `config {state:"restored",source:"bench"}`, no function run ("the automatic functions' results are in the sector"), the modes of M | PASS |
| G | the GPS wired (4 more script lines): restored, `applying {delta:true,lines:4}` with only `feature GPS`, `serial 2 +1`, `set gps_provider = UBLOX`, `set gps_ublox_nav_hz = 10`, `save`, `applied {delta:true}`; no function run; the modes of M kept | PASS |
| X | `{"type":"config","reset":"first_start"}`: the file discarded, the sector erased, the whole first start (134 lines, both calibrations), the script's modes only (M's gone), a new file | PASS |
| F | copies of X's file at a start: intact → restored; truncated, metadata not JSON, another firmware's, no metadata → ignored with a log line, the snapshot instead (and the file rewritten from it); the sector changed under its metadata → used, logged | PASS |

Notes:
- The configurator's Modes save moves slots (the UI groups ranges by mode); the core's re-read
  ~0.3 s after the last `MSP_SET_MODE_RANGE`/`MSP_EEPROM_WRITE` from the host reads them after the
  whole save (each host frame pushes the re-read back), so no partial state is sent. iNav 9.1 does
  not reboot after a Modes save; a reboot (another tab's "Save and Reboot") re-reads after the boot.
- Lines removed from the script are reported (`removed:[...]`) and left in the firmware (the
  configurator may have set them since).
- A file ignored for another firmware is replaced by that firmware's config at its first save: a
  bench keeps one config, the current firmware's.

## Gates (`gates-2026-09-30.log`, 7bb23db, homelab)
Host tests all pass (`test_bench_state` 28 checks new); `equiv` 250/250; RS7 liftoff `c5a2b9bb6c49` ×2
(9.72 / 9.95 host s per 10 virtual s); the trainer's first start from an empty cfg-cache (no bench
state: the evidence scripts pass `BENCH_STATE=`) and plane-launch `51d4a9212465` ×2 — the baselines,
unchanged.

## Review (merge-bench, 2026-09-30): the configurator's 10 s window after a reboot — proposal, not implemented
Stage E (`evidence/plane/README.md`): after a disarmed "Save and Reboot" the configurator's reconnect
gets no identification within its 10 host s (`serial_backend.js` `timeout.add('connecting', .., 10000)`,
armed in `onOpen`) because iNav's boot to its first MSP reply takes 3.6 virtual s, which is
~23.6 host s on the merged build (the speed merge, `evidence/speed-inav`: 33.5 → 23.6 s). Still > 10 s. Options:
- **The core answers MSP for the firmware during the boot:** rejected — it fakes the board (the
  identification would claim a firmware that is not running yet; the next requests would time out anyway).
- **Leave the TCP connection unaccepted until the firmware answers:** does nothing — the listening
  socket's backlog completes the handshake in the kernel, Node's `net.connect` resolves, the 10 s run
  from `onOpen` all the same.
- **Close the MSP listener while the firmware is not answering** (connect → ECONNREFUSED → the
  configurator's "Failed to open" at once): honest and fast, but needs a listener toggle on QEMU's
  socket chardev, and the configurator does not retry a refused connect — the user presses Connect
  again either way. Later, if the app's hint is not enough.
- **Recommended (cheapest, app only, no core change):** the app already has what it needs —
  `event reboot` / a start, then `state.fc.msp` false → true. While false it shows "firmware booting —
  the configurator can connect in ~N s" next to the MSP address (N = the last boot's host seconds, which
  the app measures itself from `reboot` to `fc.msp:true`), and the address's tooltip says: after a
  "Save and Reboot" wait for the green dot, then press Connect again. Nothing is faked, nothing
  changes in the core; it goes away by itself when the boot runs at ≥ ~0.5× (the speed track).

## Review gates on the merged tree (merge-bench 32c7999 = origin/main 8b4fdbd + bench-state; build `qemu-bs`, ports 31270–31274; 2026-09-30 Yerevan)
- Host suites: devices, fdm, `test_i2c` 68, `test_i2c_v1` 78, `test_msp` 39, `test_bench_state` 30, `test_prof` 3851, `test_session` 216 — 0 failed. `equiv` 250/250.
- RS7 liftoff `c5a2b9bb6c49` ×2 (8.60 / 8.40 host s per 10 virtual s).
- Trainer: first start from an empty cfg-cache, then plane-launch `51d4a9212465` ×2 (58 host s for 54 virtual s).
- Bench state S M R G on the merged build: M, R, G PASS; S writes the state from the script's save on (12 firmware saves, the first one before the script not kept).
- Bare firmware (the speedybee, no session, no script, `bench-state=` given): a CLI `set name` + `save` is kept (`"script": ""`, 3 saves) and on the next start restored ("blank config sector: the bench state written back (no script)"), `name = BARETEST`.

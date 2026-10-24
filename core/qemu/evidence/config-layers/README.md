# Evidence: the firmware's config as layers with provenance, bench lines, explicit operations (branch `config-layers`, 2026-09-30)

`scripts/config_layers_evidence.py "A S D M P N E Z"` on the homelab (QEMU `~/flightbench-spike/qemu-cl`), the
trainer (iNav 9.1.0, speedybee-f405-wing, with its GPS) on ports 31270–31274, an empty cfg-cache and one
bench-state file for the whole run. Log: `config-layers-evidence-2026-09-30.log`; stage Z again on the same
run's bench (`EV_TMP=`) after an evidence-script fix: `config-layers-evidence-z-rerun-2026-09-30.log`. The
answers as JSON: `a-config_json.json` (session-check), `s-applying.json`, `d-against.json`,
`m-against.json`, `p-applying.json`, `n-against.json`, `e-export.json`; the snapshot the core wrote:
`trainer.snapshot.cli`. Gates: `gates-2026-09-30.log`.

The bench: `core/qemu/sessions/trainer.json` + `"bench_cli": "<tmp>/bench/trainer.cli"` with
```
# the bench's own lines (the app writes this file)
set nav_fw_bank_angle = 40        <- over the aircraft's `set nav_fw_bank_angle = 35` (inav.cli:25): a conflict
set nav_fw_launch_thr = 1750
```
and `"disabled_lines": ["device:4"]` (the ublox's 4th line, `set gps_ublox_nav_hz = 10`: iNav's default instead).

| stage | what | result |
|---|---|---|
| A | `session-check`: 135 `config_src:` lines (aircraft 130, device 3, bench 2; the board's `defaults.cli` has none), `config_disabled: [device:device ublox on uart3] set gps_ublox_nav_hz = 10 (disabled_lines 'device:4')`, `config_conflict: set nav_fw_bank_angle: [aircraft:models/trainer/inav.cli:25] set nav_fw_bank_angle = 35 -> [bench:…/trainer.cli:2] set nav_fw_bank_angle = 40 (the last wins)`, the notes `session: config: disabled_lines 'device:4': … not sent` and `session: config: conflict: …: the later line wins`; `config_json` with the three layer files, 135 lines, 1 disabled, 1 conflict (`wins: "bench:1"`) | PASS |
| S | first start: `applying` carries `sent` — 135 lines, each `{text, layer, source}` (aircraft 130, device 3, bench 2); `applied` with both calibrations; `diff against:"script"` (the refresh refused while iNav's boot calibration ran — `calibrating` — asked again): 0 changed; the firmware has `set nav_fw_bank_angle = 40`, `set nav_fw_launch_thr = 1750`, and no `gps_ublox_nav_hz` (the default: the disabled line was not sent) | PASS |
| D | restart (the bench state restored): `against` with `refresh:false` (the bench state's diff, `source:"bench"`) and refreshed (`source:"request"`): 124 script settings, 145 firmware settings, **0 changed**, 33 only in the firmware (the calibration results, the blackbox flags, `acc_hardware`…), 12 only in the script (values that are iNav's defaults — `nav_fw_launch_max_angle 45`, `motorstop_on_low ON`… — and `serial 2 +1`, which the target already has) | PASS |
| M | the configurator's Modes tab Save replayed (all ranges by `MSP_SET_MODE_RANGE` in the UI's order, `MSP_EEPROM_WRITE`; bench_state_evidence.py M): the diff marked stale; `against`: `changed: aux 2: script 'aux 2 36 2 1700 2100' firmware 'aux 2 1 3 900 1300' [aircraft:models/trainer/inav.cli:178]` and 3 more `aux` only in the firmware — what the configurator changed, named with the script line (and its file:line) it overrode | PASS |
| P | (review run) the bench file edited (40 → 42, `set nav_fw_launch_idle_thr = 1100` added), `{"type":"config","apply":true}`: the bench state's diff is stale (M's configurator save), so `diff all` first (a reboot), then the bench assembled again from its files; `applying {"lines":5,"delta":true,"apply":true,"reasserted":[{"setting":"aux 2","script":"aux 2 36 2 1700 2100","firmware":"aux 2 1 3 900 1300","layer":"aircraft","source":"models/trainer/inav.cli:178"}],"removed":["set nav_fw_bank_angle = 40"],"sent":[control_profile 1, battery_profile 1 (the contexts in force), aux 2 36 2 1700 2100, set nav_fw_bank_angle = 42, set nav_fw_launch_idle_thr = 1100]}`, `diff all` (`source:"apply"`), `save`, `applied`; the firmware has 42, 1100 and `aux 2 36 2 1700 2100` again (0 changed); M's other ranges (not the script's) kept. Apply again: `applied {"lines":0,"delta":true,"apply":true}` at once, no reboot. `"full":true`: 136 lines typed (aircraft 130, device 3, bench 3). (The first run, before the review: the delta typed only the 2 bench lines and left the configurator's `aux 2`.) | PASS |
| N | `{"type":"config","snapshot":"bench","path":"<tmp>/bench/trainer.snapshot.cli"}`: `diff all` (the firmware reboots), `{"type":"config","snapshot":"written","path":…,"lines":162}`, the file (`trainer.snapshot.cli`: header, the diff's lines without blank/`batch`/`defaults`/`save`). The bench then names it as `bench_cli` (the app's step): **restart** → `restored {"source":"bench"}` then the 52 snapshot lines the sector's script did not have as the delta (the same values), the modes as before the snapshot — the bench state stays the running config; **`config reset first_start`** → the whole first start with 295 lines (aircraft 130, device 3, bench 162) + both calibrations, and the modes of the snapshot are back: the snapshot is the starting diff of a fresh first start. `against` after it: 8 changed, all calibration results (`accgain_*`, `magzero_*`, `maggain_*`, `ins_gravity_cmss`: the snapshot's values, measured again by the first start's calibrations) | PASS |
| E | `{"type":"config","export":true}`: `bench`, `root`, `assembly` (from the files now: layers board / aircraft / bench = the snapshot, 295 lines, 1 disabled, 1 conflict — `nav_fw_bank_angle` now `aircraft:…:25` → `bench:…snapshot.cli:91`), `script` (295 lines), `applied: true`, the last diff (`source:"bench"`, 268 lines); a `log` line; the core wrote nothing | PASS |
| Z | refusals: snapshot without `path` (`config snapshot: need "path": … the core writes no file it was not given`), two operations in one `config`, `against:"board"`, `apply:false`; a second `apply` while one runs → `config apply: not now: busy`; `apply` on a bench made invalid (`"wiring": 5`) → `config apply: the bench … is rejected: wiring: need a list`, nothing changed (first run of Z: FAIL, an evidence-script bug — it edited the old bench file after N had pointed the bench at the snapshot, so both applies had nothing to type; fixed, Z again on the same bench: PASS) | PASS |

## Precedence (what wins, documented in `docs/protocol.md`)
1. Within the script: layers in order — board, aircraft, profile, device, bench — the later line wins (as
   the firmware applies them); a conflict is reported, never merged or dropped.
2. The running config: the bench state (`-M bench-state=`) — the sector as the firmware last saved it. A
   script change (a new bench line, a snapshot named as `bench_cli`) reaches it as the delta at the next
   start or by `config apply`. At a start unchanged lines are not typed again (the bench state wins);
   `config apply` also re-types the settings the firmware's config has with another value than the
   script's (`reasserted`, after a fresh `diff all`), not the ones the diff does not show (the firmware's
   default: `apply full` for those).
3. A snapshot named as `bench_cli` is the starting diff: a fresh first start (`config reset` — it reads the
   bench's files again since the review —, a new bench,
   `reset all` with this script) types the layers and then the snapshot's lines; the first start's
   automatic calibrations run after the script and replace the calibration values the snapshot carries.

## Gates (`gates-2026-09-30.log`, homelab, `qemu-cl`, ports 31270–31274)
- RS7 liftoff (`sensors_evidence.py R`): **`c5a2b9bb6c49` ×2** (8.79 / 8.61 host s per 10 virtual s) — unchanged.
- Trainer (`plane_evidence.py C L`, `trainer.json`, no bench lines): first start from an empty cfg-cache (134
  lines, `applying` now with `sent`; `applied` at t = 72.9 as before), plane-launch **`51d4a9212465` ×2**
  (61 host s for 54 virtual s) — unchanged. The bench without `bench_cli`/`disabled_lines` sends the same
  script as before; `sent` rides on the control socket only.

Host suites: devices (13 tests) and fdm OK; `test_i2c` 68, `test_i2c_v1` 78, `test_msp` 39, `test_bench_state` 30,
`test_prof` 3851, `test_session` 228 (12 new: the layers API, `bench_cli`, `disabled_lines`, the conflict,
`config_json`, the blob-board refusal) — 0 failed. `equiv` through the QEMU binary: **265/265** (250 + 15 new
generated benches: bench lines with conflicts across aircraft / profile_text / device / bench, relative serial
vs absolute, `smix`/`mmix` resets, context suffixes, `disabled_lines` by text and ref, unmatched entries, NUL
bytes, a missing / forbidden / non-string `bench_cli`, a non-list `disabled_lines`, both on a blob board) and
**`cli_diff` 2/2** (the C and Python normalisers on `tests/fixtures/cli_diff/`: the trainer's script against a
real `diff all`, and an edge-case pair).

## Review (2026-09-30, second agent)
Log `config-layers-evidence-review-2026-09-30.log`: the whole run again on the reviewed code, A S D M P N E Z
all PASS; the JSON answers and `trainer.snapshot.cli` in this directory are from that run. Changed by the review:
- `cli_norm`: a relative serial bit that does not fit an int (`serial 2 +4294967297`) was bit 1 in C
  (atoi wrapped) and no bit in Python — the one mismatch of a C-vs-Python fuzz (3000 generated script/diff
  pairs from the trainer's and RS7's real diffs + mutations: contexts, relative/absolute serial, feature
  sets, numbers, case, whitespace, NUL-free junk; 0 mismatches after the fix). `rateprofile` is a context
  (RotorFlight's rate settings were filed under the last PID profile). New `edge` fixture lines.
- `config apply` re-asserts the script layer (above, P) and a delta — the apply's and the bench start's —
  types the context lines in force at each line (`control_profile 2` before a profile-2 line: before, the
  line went to the active profile). Host test `devices/tests/test_cli_norm.c`.
- A firmware save not run by the core marks the diff stale before the bench state keeps it (it was written
  with the sector before the MSP decoder marked it, so a restart restored M's pre-configurator diff as fresh).
- `config reset` reads the bench's files again (as apply), so a snapshot named as `bench_cli` needs no restart.

## Found on the way
- `run.sh` read the launch line with `sed '/^manifest: /q'`: sed quitting early SIGPIPEs `printf` under
  `pipefail` once the report is larger than a pipe buffer (64 KiB). A bench whose `bench_cli` is a snapshot
  has a 98 KB report (`config_src` + `config_json`); `run.sh` then exited 141 without a word. Fixed (sed reads
  the whole report); the app reads the whole stdout already.

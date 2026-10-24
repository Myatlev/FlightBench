# Phase 4b: speed of the iNav machine (speedybee-f405-wing), branch `speed-inav`, 2026-09-29

Three guest-invisible QEMU patches and one opt-in, measured with `scripts/speed_inav_evidence.py` and
the host-time ledger (`prof.py`, `../prof/README.md`). Host: homelab, i7-4790, Ubuntu 24.04, kernel 6.8;
the user's own core (PID 751189) and other agents' cores ran alongside (load 2–5, see each log's header).
Base = `f4d83a5` (origin/main when the branch started), final = `c9038ce` (+ scripts/docs only after it;
binary `speed-bins/sp-fix4`, identical to `qemu-sp/build`).

| host s (realtime factor) | base | final | final + `-M io-tb=on` |
|---|---|---|---|
| trainer plane-launch, host s per 10 virtual s (virtual 24..54) | 7.3 (7.29, 7.25, 8.11, 7.24): 1.37× | **4.2** (4.30, 4.27, 4.14): 2.4× | 3.6–3.9 (3.64, 3.86, 4.09): 2.6× |
| trainer boot from the snapshot, power-on → "firmware says" (t = 3.49 virtual s) | 31 (30.2–33.5): 0.11× | **21.9** (21.3–23.1): 0.16× | 20–24: 0.17× |
| reboot after the CLI `exit` → "firmware says" again (→ configurator connects) | 30.8–32.9 (31.7–33.8) | **22.7** (23.3) | 18.7–19.0 (19.1–19.5) |
| first start, empty cfg-cache → config applied (t = 46 virtual s) | 175.3 | **108.9** | – |
| bare boot (`f405-boot`), power-on → "firmware says" (t = 7.94) | 126–140 | **91–98** | 77–84 |
| bare board steady state (virtual 14..30, after its save + reboot), per 10 virtual s | 5.98 | **4.14** | 3.90 |
| main-loop wakes per virtual s (flight / snapshot boot) | 15.9 k / 3.6 k | 296 / 291 | – |
| `vcpu.bql_wait` per virtual s (flight) | 86 ms | 3.9 ms | – |
| trainer FDM log (plane-launch, 54 s) | `4f8c7114358a` | `4f8c7114358a` | `e9bf4c24732e` (new trajectory) |

Flight: **1.7× faster**; boot and reboot 1.4×; first start 1.6×. Two final-build flights in
`final-2026-09-29.log` (6.95, 10.29) ran next to another agent's bench and are left out of the table;
`alternating-2026-09-29.log` alternates base/final/io-tb in one session (round 2 was cut by a usage
limit after base) and gives the same picture.

## Steps
| step | patch | trainer flight host s / 10 vs | snapshot boot host s | FDM |
|---|---|---|---|---|
| base | – | 7.27 | 31 | `4f8c7114358a` |
| 1. USART wake storm | `patches/usart-accept-input.diff` | 5.64 | 32 | same |
| 3. ICSR without a vector scan | `patches/nvic-icsr-cache.diff` | 4.72 | 27.6 | same |
| 3b. memoized icount rewind | `patches/icount-io-tb.diff` (memo part) | 4.29 | 21.7 | same |
| (opt-in) one TB per I/O insn | `-M io-tb=on` | ~3.6 | ~20 | `e9bf4c24732e` |

1. **USART wake storm** (`diff-base-fix1.txt`, flight): the stock `stm32f2xx_usart` called
   `qemu_chr_fe_accept_input()` on every SR/DR read → `qemu_notify_event()` → a main-loop wake that takes
   the BQL from the vCPU. Now only when RXNE really clears, and not at all for the machine's own `fbwire`
   chardevs (they push their input from virtual timers; new `QEMU_CHAR_FEATURE_INPUT_PUSHED`). Socket
   chardevs (MSP host port) behave as before: the configurator connect after `exit` is served (stage R).
   `mainloop.dispatch` 15.9 k → 0.3 k per virtual s, `vcpu.bql_wait` 86 → 4.6 ms, `main.bql_wait`
   51 → 0.3 ms, USART1 935 → 65 ns per access.
2. **Idle skip for iNav**: no detector change was needed. The SysTick trigger from phase 1
   (`idle-skip-src=systick`) already locks onto the idle pass: p = 4 VAL reads per `scheduler()` pass
   (63, 60, 154, 504 instructions apart). The flight's skip count is the same on every build
   (`idle.skip` 21.0 k per virtual s, `idle_skip` 7751 in base and final: the decisions are identical).
   Bare board after its reboot (virtual 14..30, final build, `bare-steady-2026-09-29.log`): 19.04 host
   s per 10 virtual s with the skip off, 4.14 with it on (4.47 with prof=on). Phase 1 (`../f405/README.md`) is
   consistent with this once the windows are matched: its speed stage (virtual 10..20) holds the
   reboot (answers at 7.9, saves and reboots at 9.1, answers again at 12.6) and measured 49.98 → 40.36
   (1.25×); the same window on the final build gives 37.03 → 29.72 (1.25×, `bare-steady-…`). Its
   "the NVIC/SysTick reads drop only ~7 %" is a read count over the 0..20 profile, two boots of
   `delay()` spins the skip leaves alone (below). What is **not** skipped: `delay()`/`delayMicroseconds()` (`while (micros() - now < us)`,
   p = 1). Their end is a time only the firmware knows; skipping into them would move where every delay
   ends, i.e. change the guest's timing. The boot is made of them (LED/beeper flashes, `delay(500)`, the
   USB disconnect, the gyro init): 7 M ICSR + 7 M VAL reads per virtual s, which is why the boot stays
   at 0.16×. The firmware's `status` load is unchanged by construction (FDM log and skip counts
   byte-identical to base).
3. **Cheaper hot reads**: ICSR 148 → 15 ns (`armv7m.nvic`, 7 M reads per virtual s of boot: −0.94 s
   host per virtual s). The ISRPENDING/RETTOBASE bits are kept by `nvic_recompute_state()` instead of a
   scan of all vectors per read; checked against the scan at every read over two boots and a flight
   (`sp-nvicverify` build, no mismatch). The larger cost turned out to be the icount rewind itself:
   a device access mid-TB makes `cpu_io_recompile()` search the TB by host PC and decode its unwind
   data at every execution; memoized (256 entries, keyed by host PC and `tb_flush_count`), boot
   28.8 → 21.7 host s. SysTick VAL stays at ~100 ns per read (see caveats).
4. **Boot**: dominated by `micros()` polling in `delay()` loops (above); the per-read costs of steps
   1–3 are what made it cheaper. Flash programming and the config-sector erase do not show in the
   profile (`speedybee-f405-wing.flash` < 0.1 ms per virtual s). `-M io-tb=on` (one TB per device-I/O
   instruction, no rewind at all) gains another 10–15 % but changes the idle skip's decisions and with
   them the flight (`e9bf4c24732e`, deterministic): opt-in, off by default (`patches/README.md`).

## Gates (final binary)
- trainer `plane-launch` (flight.py, stage L) ×2: `4f8c7114358a` ×2 = base (`base-2026-09-29.log`,
  `final-2026-09-29.log`); every F run of every guest-invisible build: the same.
- RS7 liftoff ×2: `c5a2b9bb6c49` ×2 (`rs7-fix4-2026-09-29.log`; 9.14 host s per 10 virtual s, was 9.38
  with the USART+NVIC patches).
- `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test`, `test_wiring.py`,
  py_compile: pass. `make -C core/qemu/tests equiv`: 230/230 (see caveats).

## After the merge with main (sensors), 2026-09-30 (`merge-2026-09-30.log`)
`merge-speed` = origin/main (sensors: I2C1 + SPL06/QMC5883L, u-blox GPS on uart3, ADC1) + this branch; clean
merge. Main's core = `~/flightbench-spike/qemu-prof` (`ab6d427`, later main commits are evidence/scripts).
- Gates (merge build, `sensors_evidence.py "C L R"`, empty cfg-cache): first start applied at t = 72.889 as
  main's Linux run; trainer plane-launch **`51d4a9212465` ×2** (= main's Linux baseline); RS7
  **`c5a2b9bb6c49` ×2**; devices/fdm/core/qemu/tests suites, `test_wiring.py`, py_compile pass; `equiv`
  250/250 ×5 (3 × `bash -x` with stderr kept, 2 × `make test && make equiv`): the exit 2 below did not recur.
- Speed (`speed_inav_evidence.py "S F R"` from a clean checkout, merge / main / merge, load 3–3.7):

| host s (realtime factor) | main `ab6d427` | merge |
|---|---|---|
| trainer plane-launch, per 10 virtual s (24..54) | 11.3 (11.51, 11.14): 0.88× | **6.3** (5.94, 6.62, 5.95, 6.60): 1.6× |
| boot from the snapshot → "firmware says" (t = 3.62) | 33.5 (32.8, 34.1): 0.11× | **23.6** (22.3, 25.0, 24.7, 22.5): 0.15× |
| reboot after `exit` → "firmware says" / configurator connects | 36.2 / 37.1 | **22.5 / 23.0** (22.4 / 23.0) |

  The sensors made the flight dearer on both (7.3 → 11.3 on the old core); the patches still give 1.8× in
  flight and 1.4–1.6× on (re)boot. FDM log `51d4a9212465` on every flight of both builds.
- ICSR compare build (`speed-bins/sp-nvicverify-review`, speed-inav): 84 M reads over a bare boot with its
  reboot and two snapshot boots, 25 M over a flight (`4f8c7114358a`), no mismatch; the guest never read
  ICSR with ISRPENDING set or two exceptions active, so the nested cases rest on the code argument
  (every write of a vector's pending/active state is followed by `nvic_irq_update()`/`nvic_recompute_state()`).

## Caveats
- SysTick VAL reads still cost ~100 ns (7 M per virtual s of boot, ~10 % of the boot's vCPU time;
  ~7 % of the flight): `ptimer_get_count()` → the virtual clock under icount, plus the idle detector
  on every VAL read. Not attempted.
- The first `equiv` run on the final build (right after `make -C core/qemu/tests test`) exited 2 and
  its output was lost; 14 runs after it all passed 230/230, 5 more on the merge 250/250. Not explained;
  `make` exits 2 for any failing recipe, so the status does not say which check failed (the script's
  `QEMU … help | grep -q` under `pipefail` is not it: QEMU ignores SIGPIPE and exits 0).
- Timings on a shared host: ±5 % between runs, worse when another bench flies (left out above).

## Files
- `base-2026-09-29.log`, `fix1-…`, `fix3-…`, `fix4-…`, `final-…`: stages C/B/S/F/L/R per build;
  `fix2*`/`fix3-iotb` = io-tb on (fix2/fix2b: an intermediate build where it was the default).
- `*-prof-*.log` + `boot-prof-*.json`, `plane-launch-prof-*.json`, `bare-boot-skip-on-*.json`: prof=on
  profiles; `diff-*.txt`: `prof.py diff` (base → fix1, base → final for flight, boot, bare boot).
- `alternating-2026-09-29.log`: base/final/io-tb alternating (B S F R).
- `bare-steady-2026-09-29.log`: the bare board's speed after its reboot, skip on/off, io-tb.
- `first-start-final-2026-09-29.log`: stage C on the final build.
- `rs7-fix3-…`, `rs7-fix4-…`: RS7 speed + determinism (`speed_evidence.sh`).

## Reproduce
```bash
core/qemu/apply.sh ~/flightbench-spike/qemu-sp && core/qemu/build.sh ~/flightbench-spike/qemu-sp
QEMU=…/qemu-system-arm core/qemu/scripts/speed_inav_evidence.py "C B S F L R" final
B_FROM=14 B_UNTIL=30 SKIP=off core/qemu/scripts/speed_inav_evidence.py B steady-off
MACHINE_OPTS_EXTRA=io-tb=on core/qemu/scripts/speed_inav_evidence.py "B S F R" iotb
```

# Speed fixes in the nexus-xr core (branch `speed`, 2026-09-28)

Three fixes aimed at the vCPU ↔ main-loop hand-offs found by the host-time ledger
(`../prof/README.md`), each measured on the RS7 liftoff (session `sessions/rs7.json`, plan `liftoff`,
45 virtual s) with `scripts/speed_evidence.sh`. Host: homelab, i7-4790, Ubuntu 24.04, kernel 6.8; the
user's own core (another `qemu-system-arm`) ran alongside during every run (load ~2).

| | main (fix0) | + fix 1 | + fix 2 (branch tip) |
|---|---|---|---|
| prof=off, host s per 10 virtual s (virtual 5..45, 3 runs) | 16.20 (16.13, 16.31, 16.15) | 12.81 | **9.49** (9.46, 9.42, 9.59) |
| hand-offs (vCPU slice ends) per virtual s | 30 823 | 30 823 | 22 082 |
| main-loop wakes per virtual s | 25 772 | 347 | 347 |
| FDM log (md5, 45 s) | `d66fd7a91d0f` | `d66fd7a91d0f` (byte-identical) | `c5a2b9bb6c49` (flight equivalent) |

**Main → tip: 1.71× faster** (−41 % host time) on this host. Fix 3 (gyro) was tried in two variants,
both slower; it is not in the tip (two WIP commits and a revert on the branch, see below).

## How to reproduce
```bash
core/qemu/build.sh ~/flightbench-spike/qemu-prof            # a tree with patches/prof-qemu.diff
cp ~/flightbench-spike/qemu-prof/build/qemu-system-arm /some/dir/tip/   # keep one binary per build
core/qemu/scripts/speed_evidence.sh "P S E Q T I" main=/path/main/qemu-system-arm tip=/some/dir/tip/qemu-system-arm
```
Stages (ports 31280..31284): **P** prof=on liftoff to 45 s per build → `rs7-liftoff-<host>-<date>-<label>.json`
+ `prof.py diff` against the previous build; **S** prof=off speed, `RUNS` (3) per build, alternating;
**E** FDM log md5 of every run; **Q** flight equivalence against the first build (lift-off time,
roll/pitch/yaw at 35/40/45 s, head speed airborne, altitude at 45 s); **T** CLI `tasks` + `status` on the
pad at t > 15 virtual s; **I** IMU trace (`-M imu-log=`, builds that have it).

Files: `rs7-liftoff-homelab-2026-09-28-fix{0,1,2}.json/.txt` (fix0 = main), `…-fix3-wip-lazy-dma` and
`…-fix3-wip-lazy-pulse` (the rejected fix 3 variants), `diff-fix0-fix1.txt`, `diff-fix1-fix2.txt`,
`diff-fix2-fix3-wip-*.txt`; logs `speed-fix1-…log` (fix0 vs fix1: P S E Q T), `speed-fix2-P-…log`,
`speed-fix2-…log` (fix1t vs fix2: S E Q T I), `speed-fix3-wip-*-P-…log`, `speed-final-…log` (main vs
tip: S E Q T), `speed-final-imu-…log` (I), `faults-{fix0,final}-…log` (`fault_evidence.py`).
`fix1t` = fix 1 + the `imu-log=` trace commit (guest-invisible: its FDM log is main's md5), the
reference for the IMU trace since main has no trace.

## Fix 1 — no main-loop wake for nothing (QEMU patch; guest-invisible)
`patches/prof-qemu.diff` gains `qemu_clock_notify_nonempty()` (`util/qemu-timer.c`): the icount
deadline path (`icount_notify_aio_contexts()`) notifies the main loop's timer list as before
(`qemu_timer_notify_cb()` untouched) and an AioContext's `QEMU_CLOCK_VIRTUAL` list only when
`timerlist_has_timers()`; replay mode keeps `qemu_clock_notify()`. `git apply --check` on a clean
v11.1.1 tree OK; `apply.sh` stays idempotent and now replaces an older applied version of a patch.

| RS7 liftoff, per virtual s (prof=on) | fix0 (main) | fix1 |
|---|---|---|
| deadlines by timer: `crsf.frame` / `dma2.done` / `imu.pulse_end` / `imu.sample` / `esc.reply` / SysTick / `fdm.tick` | 9 718 / 5 786 / 3 962 / 3 962 / 1 910 / 1 000 / 1 000 | the same |
| hand-offs (`timer_deadline` / `idle_skip` / `timer_rearm`) | 30 823 (7 434 / 20 479 / 2 910) | 30 823 (the same) |
| vCPU → main-loop wakes (`aio_notify` → poll returns) | 25 772 | **347** (bottom halves / socket output; none for the machine's timers) |
| main-loop iterations | 25 841 | 409 |
| `vcpu.bql_wait` | 161.9 ms (25 530 contended) | **5.4 ms** (381) |
| `main.bql_wait` | 56.1 ms | 0.2 ms |
| `mainloop.dispatch` | 167.6 ms | 4.7 ms |
| `vcpu.between_slices` | 106.5 ms | 45.5 ms |
| headline (prof=on, boot included) host s / 10 virtual s | 18.83 | 15.15 |
| prof=off, host s / 10 virtual s (virtual 5..45), 3 runs alternating | 16.39, 16.33, 15.97 (mean 16.23) | 12.59–13.12 (mean **12.81**, −21 %) |
| determinism | FDM log `d66fd7a91d0f` in all 4 runs | `d66fd7a91d0f` in all 4 runs: **byte-identical to main** |

Every guest-side count in the diff (MMIO accesses per region, timer firings, deadlines, idle skips) is
identical: the guest cannot tell. Task stats on the pad: the same set of tasks with `late` > 0.

## Fix 2 — CRSF: one deadline per frame (machine; flight equivalent)
The receiver's `crsf.frame` timer fired once per 420 kbaud byte plus a 250 µs idle poll. Now it fires at
the frame times only (`fb_crsf_next_due_ns()`: the model's 250 Hz RC grid and the 200 ms link-statistics
grid; pilot commands keep their own wake-up): the first byte goes into RDR at the frame time and each
next one is released right after the firmware's RX interrupt has consumed the previous one —
RotorFlight's handler (`serial_uart_hal.c` `uartIrqHandler`) reads RDR and then writes `RQR.RXFRQ`;
a thin wrapper on UART5's MMIO ops feeds the next byte after that write, so the flush cannot eat it.
Bytes not read by the next frame time are dropped (`crsf_dropped` in `bench`): 7 184 in the RS7 run,
all before t = 2 virtual s (6 320 at t = 1 s, 7 184 at 2, 5 and 45 s) — the boot, before the firmware
enables UART5; main loses the same bytes into the disabled USART. None from then on.
Host test: `devices/tests/test_crsf.c` (paced: 250 RC + 5 link-statistics frames per second on their
grids, one deadline per frame, whole frames per burst, stale frame dropped, link lost / back).

Review (2026-09-28): as first committed, the next byte was released *only* by the RXFRQ write; a
driver that does not write RXFRQ (polling, another HAL; DMA RX is not modelled by QEMU's USART at all)
would have received the first byte of every frame and nothing else. The release is now time-driven
with the RXFRQ write as an earlier trigger (`crsf_arm()`: byte n at frame time + n × 23.8 µs; RDR still
full at that time = overrun, the rest waits for RXFRQ or the next frame; receiver off = dropped at once),
and the frame-time discard no longer runs on pilot-command ticks. RotorFlight never reaches the fallback:
FDM log still `c5a2b9bb6c49`, speed unchanged (9.68 vs 9.67, 2 runs each). With the RXFRQ trigger
compiled out (standing in for a driver without it) the fallback flies the same liftoff, flight
equivalent to main (lift-off 33.82 s, firmware RX rate 250, fault suite 10/10; 10.43 host s per 10
virtual s, 1 run: a deadline per byte again for such a driver); the committed fix 2 in that setup does
not fly (no lift-off, head speed 0, firmware RX rate 9). Log: `review-crsf-fallback-homelab-2026-09-28.log`.

Note: the brief said 150 Hz; the receiver model runs at **250 Hz** (`fb_crsf` default, firmware
`status`: RX rate 244–260), unchanged by this fix.

| RS7 liftoff, per virtual s (prof=on) | fix1 | fix2 |
|---|---|---|
| `crsf.frame` deadlines | 9 718 | **250** (all on the 4 ms grid, where `fdm.tick` is due too: `fdm.tick` 1 000 → 750 as the earliest-due timer) |
| other deadlines (`dma2.done` / `imu.*` / `esc.reply` / SysTick) | 5 786 / 3 962+3 962 / 1 910 / 1 000 | the same |
| hand-offs (`timer_deadline` / `idle_skip` / `timer_rearm`) | 30 823 (7 434 / 20 479 / 2 910) | **22 082** (5 456 / 12 740 / 3 887) |
| wakes | 347 | 347 |
| `vcpu.bql_wait` | 5.4 ms | 4.9 ms |
| CYCCNT reads (the idle loop) / idle-skipped virtual time | 1.24 M / 74.6 % | 474 k / 77.9 % |
| USART5 accesses (the firmware's RX work) | 128.1 k | 128.1 k |
| headline (prof=on, boot included) | 15.15 | 12.11 |
| prof=off, 3 runs alternating (`fix1t` vs fix2) | 12.88, 12.71, 12.82 (mean 12.80) | 9.53, 9.65, 9.31 (mean **9.50**, −26 %) |
| determinism | — | FDM log `c5a2b9bb6c49` in all runs of the fix 2 build (P + 3× S) and of the tip build (3× S): byte-identical to each other |

Flight equivalence (FDM log, the tip vs main; fix2 vs fix1t gives the same table):

| | main | tip | criterion |
|---|---|---|---|
| lift-off | 33.82 s | 33.82 s | ±0.1 s |
| roll / pitch / yaw at 35 s | 0.00 / 0.00 / −4.05° | 0.00 / 0.00 / −4.05° | ±1° |
| roll / pitch / yaw at 40 s | 0.01 / 0.01 / −3.98° | 0.01 / 0.01 / −3.98° | ±1° |
| roll / pitch / yaw at 45 s | 0.49 / −0.47 / −4.09° | 0.49 / −0.47 / −4.09° | ±1° |
| head speed airborne | 1793..1802 rpm | 1793..1802 rpm | 1793–1802 |
| altitude at 45 s | 2.311 m | 2.310 m (−0.04 %) | ±2 % |

The logs first differ at line 324 (t = 6.46 s, on the pad).

Firmware health on the pad (CLI at t > 15 virtual s; a snapshot taken at a host-timed moment — two
guest-identical builds, main and fix1, read 8.5 % and 7.6 %, so ~1 % is snapshot noise):

| | main | tip |
|---|---|---|
| tasks with `late` > 0 | ACC 2, RX 2, DISPATCH 2, BATTERY_VOLTAGE 1, BARO 2, TELEMETRY 7, OSD 2 | RX 2, BARO 2, TELEMETRY 8, OSD 1 — **no new late task** |
| Total (excluding SERIAL) / `status` CPU | 8.4 % / 8 % | 7.6 % / 8 % (within ±2 %) |
| GYRO rate, RX rate | 4000, 244 | 4000, 250 |

Receiver faults (`fault_evidence.py`, the whole suite, host-timed): main and tip **10/10 PASS** —
`crsf.freeze` (firmware channels frozen, armed, no RXLOSS), `crsf.lq` 30 (link statistics reach the
firmware: RSSI 550 → 275 → 550, decoded LQ 100 → 30 → 100), `crsf.link_lost` (failsafe on after
0.134 s main / 0.123 s tip, off after 0.035 / 0.024 s; the fault instants are host-timed, so ~10 ms
of that is the script). Telemetry direction (FC → receiver, `crsf_rx_frames` in `bench`): 1 frame by
t = 5 s and still 1 at 45 s of the liftoff, in main and in the tip — this firmware config sends almost
no CRSF telemetry, so this check is weak; the TX path (`crsf_wire_tx`) is not touched by the fix.
There are no byte-level CRSF fault injections in the model (its faults are `link_lost`, `freeze`,
`lq`), so none to re-check.

IMU trace (`speed-final-imu-…log`): the tip samples on the same 4 kHz grid as main, **shifted by a
constant 183.9 µs** (the firmware programs the gyro's ODR at a slightly different moment of its boot,
as the CRSF bytes now arrive in bursts), 178 275 samples in 45 s in both, the same event order per sample
(INT1 rise, INT1 fall +8.000 µs, both SPI1 DMA streams done +8.876 µs typical).

## Fix 3 — gyro: one deadline per sample (rejected: slower)
Each 4 kHz sample costs three deadlines: `imu.sample` (INT1 rises), `imu.pulse_end` (+8 µs),
`dma2.done` (the SPI1 transfer started by the firmware's EXTI handler, +8.9 µs after the sample).

| variant (prof=on, host s / 10 virtual s, virtual 5..45) | deadlines per sample | hand-offs / virtual s | CYCCNT reads / virtual s | idle-skipped | speed |
|---|---|---|---|---|---|
| fix2 (all three on timers) | 3 | 22 082 | 474 k | 77.9 % | 9.94 |
| WIP A: INT1 fall and DMA completion lazy (applied at the next machine MMIO access — incl. NVIC/SysTick/USARTs — machine timer or sample; idle skip bounded by them) | **1** | 14 246 | 2.17 M | 74.9 % | **15.14** |
| — the same with only the INT1 fall lazy (temporary switch) | 2 | — | 513 k | 77.9 % | 10.19 (prof=off 9.73, 1 run, vs 9.50) |
| — the same with only the DMA completion lazy | 2 | — | 2.17 M | 74.9 % | 15.19 |
| WIP B: only the INT1 fall lazy (settled on a GPIOB IDR read or the next sample), idle skip not bounded by it | 2 | 21 997 (`timer_rearm` 3 887 → 7 763) | 1.48 M | 76.7 % | **12.77** |

Why:
- **A lazy DMA completion is late.** After its EXTI handler the firmware runs RAM-only code, so the
  first access after the SPI time comes +1.08 µs typical (p90 +1.35 µs, max +10.5 µs; trace). The
  order is kept (the sample instants were identical to fix2's), but RotorFlight's scheduler adapts
  its busy-wait before the gyro task (`schedLoopStartCycles`, 1..12 µs, widened whenever it arrives
  late) and that single-read spin is not idle-skippable: CYCCNT reads ×4.6, host time +52 %.
- **The INT1 pulse-end deadline stops the idle skip just before the scheduler's gyro target.**
  Without it (WIP B) the skip jumps past the target, the firmware widens the busy-wait again, and
  the DMA timer armed from the EXTI handler cuts the now longer slice (`timer_rearm` doubles), so the
  hand-offs do not even drop. Bounding the skip at the lazy pulse end keeps it neutral (10.19 vs 9.94
  prof=on), i.e. no gain for the extra mechanism, and it is not bit-identical either (FDM md5
  `20f22acef473` vs fix2's `c5a2b9bb6c49`).

So the tip keeps the gyro's three deadlines. The lesson is in `docs/architecture.md` ("Deadlines cost
too"): a deadline that shapes where the idle skip stops is not free to remove.

## Tests
`make -C devices test` (test_crsf with the new paced checks), `make -C fdm test`,
`make -C core/qemu/tests test` (test_i2c 68 checks, test_prof 3851 checks + JSON), `test_wiring.py`,
`py_compile` of `core/qemu/scripts/*.py` and `tools/*.py`: all OK on the tip.

## The same fixes on the other two hosts (2026-09-28, RS7 liftoff 45 virtual s, prof=on)
Files: `rs7-liftoff-{win5800x,wsl5800x}-2026-09-28-tip.json/.txt`, `diff-main-tip-{win5800x,wsl5800x}-…`. "Before" = the `evidence/prof/` profiles of `main` (fbprof merge); "after" = the `speed` tip (fix 1 + fix 2 with the reviewer's time-driven CRSF release). Steady window: homelab 5..45 s, the 5800X runs 20..45 s (same window before/after per host); the headline includes boot.

| host | steady before → after, host s / 10 virtual s | headline before → after | main-loop iterations / virtual s |
|---|---|---|---|
| i7-4790, Ubuntu (homelab) | 16.74 → 9.94 (1.68×)¹ | 18.77 → 12.11 | 25 789 → 397 |
| Ryzen 7 5800X, Windows 11 native | 14.16 → **7.40** (1.91×) | 15.29 → 9.47 | 23 522 → 386 |
| Ryzen 7 5800X, WSL2 | 16.23 → **6.06** (2.68×) | 18.15 → 7.49 | 20 010 → 357 |

¹ homelab "after" is `rs7-liftoff-homelab-2026-09-28-fix2.json`, fix 2 before the reviewer's time-driven CRSF release (no prof=on profile of the tip on homelab; its prof=off speed is the same as fix 2's, 9.68 vs 9.67, "Fix 2" above).

Hand-offs 30 823 → 22 3xx per virtual second on the 5800X tip, 22 082 on homelab fix 2 (CRSF deadlines 9 718 → 255 and 250). The hosts that paid most per wake (Windows: BQL wait 17 µs; WSL2: hypervisor wake 33 µs) gained most; the 5800X is now well above real time in lockstep on both (1.35× native, 1.65× WSL2). Flight by hand in real time on Windows is within reach without a Linux box.

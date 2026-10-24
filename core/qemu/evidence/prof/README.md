# Host-time profiles of the core (`-M prof=on`, branch `prof`, 2026-09-28)

The ledger (`hw/arm/fb_prof.c.inc`, machine section "Host-time ledger" in `hw/arm/nexus_xr.c`,
QEMU hooks `patches/prof-qemu.diff`) says, per run, where the host's time went: MMIO regions,
machine timers, socket/wire paths, control server, FDM, and the vCPU ↔ main loop hand-offs with
their reason and latency. The message format is `prof` in `docs/protocol.md`.

## How to produce a profile
```bash
core/qemu/build.sh ~/flightbench-spike/qemu-prof          # a tree with patches/prof-qemu.diff (apply.sh applies it)
export QEMU=~/flightbench-spike/qemu-prof/build/qemu-system-arm
PROF=1 core/qemu/run.sh --session core/qemu/sessions/rs7.json &      # the core, ledger on
python3 core/qemu/scripts/prof.py run --session core/qemu/sessions/rs7.json \
    --plan liftoff --until 45 --out rs7.json                          # flies flight.py's plan, pauses at 45 s, dumps
kill %1
python3 core/qemu/scripts/prof.py show rs7.json                       # the table
```
- `prof.py run` starts nothing: it talks to the running core (control port from the session, pilot
  port `--pilot`/`PILOT_PORT`), turns its own state stream off, queues the plan (`flight.plans()`),
  steps the core to `--until` (it pauses exactly there), watches the clock with `ping` (20 Hz) and
  writes the `prof` message plus `meta` (git hash, host name, session, plan, ping-watched speed from
  `--speed-from` to `--until`). No `--plan`: the firmware just runs (bare boot).
- The ledger counts from machine init (boot included) until the dump; `{"type":"prof","reset":true}`
  starts it again. The `show` headline (`wall/virt`) therefore includes the boot; the `meta.speed` line
  is the steady part (virtual 5..45 s).
- Without a script: `{"type":"prof"}` on the control socket (`tools/core_client.py send
  '{"type":"prof"}' --wait 1`), or `PROF_PERIOD=5` for one `prof` message every 5 virtual seconds.
- `core/qemu/scripts/prof_evidence.sh [session] [stages]` does everything below on its own ports
  (31280..31284), `RUNS=5` for more overhead runs.

## How to compare two
```bash
python3 core/qemu/scripts/prof.py diff homelab.json windows.json      # or before.json after.json, fwA.json fwB.json
```
Every row is per **virtual** second (runs of different length compare): µs of host time and events
per virtual second in A and B, delta, ratio, ns per event; `*` marks the 5 largest |delta| (`--movers N`).
Then hand-offs per virtual second by reason and by timer, wake/kick latency (mean/p50/p99), main-loop
iterations, and the headline host s per 10 virtual s with its ratio. For Windows vs homelab: run the
same session and plan on both, copy one JSON over, `diff`. The `host` object (CPU, OS/kernel — WSL
shows `microsoft` in the kernel string — QEMU version, FlightBench build) is printed on top.

## Reading the numbers
- **Threads.** The vCPU thread runs guest code, MMIO handlers and (under `-icount`) all virtual
  timers; the main loop thread polls sockets and runs their handlers. They run in parallel, so the
  `% wall` column of accounts on different threads does not add up to 100.
- **vCPU thread = accounted + `guest_exec_or_unaccounted`.** Accounted: MMIO handlers, the time
  between two slices (`vcpu.between_slices`, which contains the timers it runs), BQL waits. The rest
  is guest code, TB lookup/translation and QEMU's own exec-loop bookkeeping — not split further (no
  guest profiler, by design).
- **`host` vs `self`.** An account's host time includes accounts nested inside it (`fdm.tick`
  contains `fdm.step`, `esc.step`, `ctl.tick`); `self` does not.
- **Sampled accounts.** `armv7m.nvic`, `armv7m.systick`, `nexus-xr.dwt` (`"sampled":7`): every access
  is counted, every 7th is timed and counts 7 times — these three see ~2 M accesses per virtual second
  and timing each doubled the ledger's overhead. Their handlers enter no other account.
- **Hand-off = one end of a vCPU slice** (`tcg_cpu_exec` returned to the round-robin loop). Reasons:
  `timer_deadline` (the icount budget ran out on executed instructions: a virtual timer is due),
  `idle_skip` (it ran out through an idle skip — the skip jumps to the deadline), `timer_rearm` (the
  vCPU armed a virtual timer earlier than its budget end; QEMU cuts the slice to recompute it),
  `io` (another thread kicked the vCPU: an interrupt or timer from a socket handler, queued work such
  as the realtime throttle), `halt` (guest WFI), `timer_deadline_realtime`, `other`. `by_timer` names
  the machine timer that was due for the two deadline reasons; `not_machine` = none of the machine's
  (QEMU's SysTick, 1000/virtual s).
- **`mmio_bql` is not a slice end.** Every MMIO access takes the BQL inside the slice (TCG's
  `prepare_mmio_access`); what matters is when it is taken by the main loop at that moment: those
  contended acquisitions are `vcpu.bql_wait` (count, time blocked). `main.bql_wait` is the other side.
- **Wake latency** (`wake_ns`): the vCPU thread writes the main loop's event notifier
  (`aio_notify`, from `icount_notify_aio_contexts` → `qemu_clock_notify` at every due virtual timer) →
  the main loop's poll returns. **Kick latency** (`kick_ns`): another thread kicks the vCPU → the
  vCPU leaves `cpu_exec` / wakes / starts its next slice.
- Determined as `other` and not guessed: a slice that ends with budget left, no kick from another
  thread and no timer re-arm (exceptions leaving `cpu_exec`, the budget cap). In these runs: 0.

## Evidence (homelab, i7-4790, Ubuntu 24.04, kernel 6.8; `prof-evidence-homelab-2026-09-28.log`)
The user's own core (another `qemu-system-arm`, ~1.1 CPU) ran on the same host during all runs.

| check | result | files |
|---|---|---|
| A tests | `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test` (`test_i2c` 68 checks, `test_prof` 3851 checks + JSON parsed by Python), `test_wiring.py` OK, `py_compile` of all scripts OK; `prof.py show`/`diff` on the saved files run | log |
| B RS7 liftoff, 45 virtual s, prof=on | lifts off as before; ping-watched speed 16.74 host s / 10 virtual s (5..45 s); profile below | `rs7-liftoff-homelab-2026-09-28.json`, `.txt` |
| C bare boot (no plan, disarmed on the pad), 20 virtual s | 16.42 host s / 10 virtual s (5..20 s); 30 157 hand-offs / virtual s (flight: 30 823) | `rs7-bare-boot-homelab-2026-09-28.json`, `.txt` |
| D overhead, 5 runs each, alternating, liftoff to 45 s, host s / 10 virtual s over 5..45 s | prof=off 16.04, 16.14, 16.01, 16.09, 15.98 (mean 16.05, sd 0.06); prof=on 16.61, 16.83, 17.05, 16.70, 16.55 (mean 16.75, sd 0.20): **+4.3 %**; a rerun with HEAD +2.5 %; pooled +3.5 % — over the 3 % target, see "Overhead" | log |
| E determinism, FDM log every 20 ms to 45 s | all 11 runs (B, 5× off, 5× on) md5 `d66fd7a91d0f`, 2251 rows: **prof=on byte-identical to prof=off**; the same md5 in every other set (40 runs in all, `overhead-runs-…log`) | log |

### Overhead
RS7 liftoff to 45 virtual s, host s per 10 virtual s over virtual 5..45 (ping-watched), prof=off and
prof=on runs alternating, the user's core running alongside (`overhead-runs-homelab-2026-09-28.log`):

| ledger build | prof=off | prof=on | overhead |
|---|---|---|---|
| every access timed (a7ad9f5) | 16.16, 15.82, 16.06 | 16.82, 16.61, 16.74 | +4.4 % |
| NVIC/SysTick/DWT sampled 1/7 (be5e8df) | 16.24, 16.25, 16.10 | 16.52, 16.75, 16.60 | +2.6 % |
| same (c6080c0) | 16.37, 15.98, 15.95 | 16.74, 16.77, 17.17 | +4.9 % |
| + slice_end without locked deadline walks (1b3b615, the files B/C) | 16.04, 16.14, 16.01, 16.09, 15.98 | 16.61, 16.83, 17.05, 16.70, 16.55 | +4.3 % |
| + 64-byte lines per account/thread (9f9873c) | 15.97, 16.22, 16.81, 17.15, 16.13 | 16.92, 16.91, 16.84, 16.89, 16.78 | +2.5 % |

All 16 runs each with sampling: mean 16.21 vs 16.79 (**+3.5 %**), median 16.12 vs 16.78 (+4.1 %).
**The < 3 % target is not met reliably**: ~0.6 host s per 10 virtual s, run-to-run noise ±0.3.
Where it goes (split experiment, temporary knob, 2 runs each, virtual 5..30 s): off 16.30, full ledger
16.90, without MMIO wrapping 16.30, without the QEMU hooks 16.17 (the vCPU thread is then never
tagged, so the MMIO wrappers only pass through). So the cost is the *timed* MMIO path: ~0.85 M timed
accesses per virtual s (DMA2 244 k, USART5 128 k, TIM1 69 k, SPI1 39 k, USART1 36 k, …, plus 1/7 of
the ~2 M NVIC/SysTick/DWT accesses), two rdtsc and a stack push/pop each. The hooks and the
count-only path are within the noise. Not done: sampling the non-leaf regions too (their handlers
enter other accounts, so scaling needs care), or a global 1-in-N timing mode.

## First findings (RS7 liftoff, 45 virtual s, `rs7-liftoff-homelab-2026-09-28.json`)
- **Hand-offs: 30 823 per virtual second** (1 387 033 in 45 s). By reason: `idle_skip` 20 479/s
  (66.4 %), `timer_deadline` 7 434/s (24.1 %), `timer_rearm` 2 910/s (9.4 %), `io` 0, `halt` 0,
  `other` 0. The bare boot on the pad has 30 157/s: the flight adds 2 %.
- **Which timers set the deadlines** (27 913/s for the two deadline reasons): `crsf.frame` 9 718/s
  (34.8 % — the CRSF receiver clocks one byte into UART5 per 420 kbaud byte time, one timer firing per
  byte), `dma2.done` 5 786/s (20.7 %, SPI1 and DShot DMA completions), `imu.pulse_end` 3 962/s and
  `imu.sample` 3 961/s (14.2 % each, the 4 kHz gyro: sample, then the end of the INT1 pulse),
  `esc.reply` 1 910/s (6.8 %), SysTick (`not_machine`) 1 000/s, `fdm.tick` 1 000/s, `usart1.pump`
  349/s, `i2c3.timer` 196/s, `baro.sample` 32/s.
- **Every due virtual timer wakes the main loop**: 25 720 wakes per virtual second (≈ one per
  deadline slice; QEMU's `icount_notify_aio_contexts` notifies the main AioContext even though the
  vCPU runs the timers itself). **Wake latency on this host: mean 7.5 µs, p50 4.2 µs, p99 39.9 µs**
  (max 6.8 ms). The main loop then runs 25 789 iterations per virtual second, 10 fds per poll, with
  nothing to do in almost all of them (dispatch 6.5 µs per iteration including its BQL wait).
- **The woken main loop takes the BQL from the vCPU**: 25 459 contended acquisitions per virtual
  second on the vCPU thread, 6.3 µs each = **160.7 ms per virtual second (8.6 % of wall)**; the main
  loop in turn waits 15 632 times per virtual second, 3.6 µs each. The vCPU thread's own time between
  two slices is 2.4 µs self (3.5 µs with the timers it runs), 30 823 times per virtual second.
  Together a hand-off costs the vCPU thread ~7.6 µs on average: ~235 ms of the ~1.67 s of host
  time per virtual second.
- **Top 5 accounts by host time**: `mainloop.poll` 76.8 s (main loop thread, asleep in poll: 90.9 %
  of wall), `mainloop.dispatch` 7.57 s (main, 9.0 %), `vcpu.bql_wait` 7.23 s (vCPU, 8.6 %),
  `vcpu.between_slices` 4.87 s (vCPU, 5.8 %), `nexus-xr.dwt` 3.12 s (vCPU, 3.7 %: 1.24 M CYCCNT reads
  per virtual s, 56 ns each, the idle-skip detector included). On the vCPU thread alone the next are
  `armv7m.nvic` 3.03 s (370 k accesses/virtual s, 182 ns each — the ICSR read in `micros()`),
  `armv7m.systick` 1.49 s (383 k/virtual s, 87 ns), then `nexus-xr.dma2` 0.45 s, `nexus-xr.spi1`
  0.42 s, `TIM1` 0.34 s, `imu.sample` 0.32 s, `crsf.frame` 0.25 s, `fdm.tick` 0.20 s (of which
  `fdm.step` 0.10 s: the physics is 2.2 µs per step).
- **Accounted ratio (vCPU thread): 24.9 %**; `guest_exec_or_unaccounted` = 63.5 s of 84.5 s wall
  (75.1 %) — guest code plus TCG's exec loop. The FDM, the control server (`ctl.tick` 1.7 µs per ms,
  no state stream in these runs) and the socket paths are each below 0.3 % of wall.
- Headline of the whole ledger (boot included): 18.77 host s per 10 virtual s; steady part 5..45 s:
  16.74.

## Review note: why the main loop wakes
Checked against the QEMU v11.1.1 source (`accel/tcg/tcg-accel-ops-icount.c`, `util/qemu-timer.c`,
`util/async.c`, `system/cpu-timers.c`), because the next optimization depends on it.

- **Who notifies.** Under `-icount` the vCPU thread runs the virtual timers itself: in the
  round-robin loop, `icount_handle_deadline()` (and `icount_prepare_for_run()` when the budget is 0)
  sees a deadline of 0 and calls `icount_notify_aio_contexts()`, which is
  `qemu_clock_notify(QEMU_CLOCK_VIRTUAL)` followed by `qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL)`.
  `qemu_clock_notify` calls `timerlist_notify()` on **every** `QEMU_CLOCK_VIRTUAL` timer list,
  unconditionally, whether or not that list has a timer:
  - the main-loop timer list group (`main_loop_tlg`, where `timer_new_ns()` puts all the machine's
    timers): its notify callback is `qemu_timer_notify_cb()`, which under icount on the vCPU thread
    does `cpu_exit(current_cpu)` — no main-loop wake, but one more pass of the round-robin outer loop
    (`bql_unlock`, replay mutex, `bql_lock`, deadline walk) before the next slice;
  - the timer list of each AioContext (`qemu_aio_context` and `iohandler_ctx` in this core): its
    callback is `aio_timerlist_notify()` → `aio_notify(ctx)`. While the main loop sleeps in its glib
    poll, `aio_ctx_prepare()` has set `ctx->notify_me`, so `aio_notify` writes the event notifier and
    the poll returns. **This is the wake**: 25 720 per virtual second in the RS7 flight.
  `qemu_notify_event()` is *not* on this path (it is used when all vCPUs are idle, and by
  `qemu_timer_notify_cb` without icount).
- **Is it needed for correctness?** Only for `QEMU_CLOCK_VIRTUAL` timers that live on an
  AioContext's own timer list (`aio_timer_new`/`aio_timer_init`: block-layer throttling, qed,
  coroutine sleeps on the virtual clock). Under icount `qemu_clock_use_for_deadline(VIRTUAL)` is
  false, so no poll timeout covers them; this notify is the only thing that makes their context run
  them. The machine's own timers do not need it: the vCPU runs `main_loop_tlg` right after, under
  the BQL, before its next slice. This core registers no AioContext virtual timer (no block devices;
  the chardevs use realtime/host clocks), so every one of these wakes finds nothing to do.
- **If the notify were skipped when the context's virtual timer list is empty**
  (`timerlist_has_timers(ctx->tlg.tl[QEMU_CLOCK_VIRTUAL])` false, or its deadline not 0): the main
  loop would stay in poll until real work arrives. Bottom halves (`qemu_bh_schedule` → `aio_notify`),
  fd activity and realtime/host timers (poll timeout) wake it by themselves, so "the main AioContext
  has no pending BH/fd work" is not the condition to test — the per-list timer check is. Guest
  behaviour would not change: virtual time only advances inside vCPU slices and all due
  `main_loop_tlg` timers run on the vCPU thread before the next slice, whichever thread would
  otherwise have run them. Expected gain: the ~25.7 k wakes/virtual s, the main loop's contended
  BQL grabs that follow them (`vcpu.bql_wait` 161 ms and `main.bql_wait` 57 ms per virtual s) and
  most of `mainloop.dispatch`. A caveat for that change: if some AioContext virtual timer ever
  appears, skipping must stay per list, and replay mode (`-icount rr=`) must keep the old path.

Implemented on branch `speed` (`qemu_clock_notify_nonempty()` in `patches/prof-qemu.diff`): wakes
25.7 k → 0.35 k per virtual s, `vcpu.bql_wait` 161 → 5 ms per virtual s, FDM log byte-identical,
21 % faster (`../speed/README.md`).

## Three hosts, one workload (2026-09-28, RS7 liftoff 45 virtual s, prof=on, no client polling)
Files: `rs7-liftoff-{homelab,win5800x,wsl5800x}-2026-09-28.json/.txt`, `diff-homelab-vs-win5800x-…`, `diff-win5800x-vs-wsl5800x-…`.
Event counts are identical on all three hosts (deterministic core), so the diffs isolate the per-event cost.

| host | steady (homelab 5..45 s, 5800X 20..45 s), host s / 10 virtual s | vcpu.bql_wait per event | vCPU→main wake mean/p50 | vcpu.between_slices | DWT read |
|---|---|---|---|---|---|
| i7-4790, Ubuntu (homelab) | 16.74 | 6.3 µs | 7.5 / 4.2 µs | 3.5 µs | 56 ns |
| Ryzen 7 5800X, Windows 11 (MSYS2 build) | **14.16** | 17.3 µs | 7.7 / 6.0 µs | 3.4 µs | 31 ns |
| Ryzen 7 5800X, WSL2 (homelab binary) | 16.23 | 14.0 µs | 33.2 / 30.2 µs | 13.6 µs | 32 ns |

- Without a polling client the 5800X under Windows is the fastest host; the earlier "Windows is slowest" numbers
  (18.4 vs 15.7 with `flight.py`) were dominated by the client's MSP/HMP polling over TCP loopback, which is
  expensive on Windows. Measure the core with `prof.py run` (ping-watched), not with a polling client.
- Windows pays for the hand-off: the contended BQL wait after each main-loop wake costs 17.3 µs per event,
  405 ms per virtual second (29 % of host time) — the target of "skip the empty main-loop wake" (review note above; done on `speed`, the 5800X before/after in `../speed/README.md`).
- WSL2 pays in the hypervisor: wake latency 33 µs mean, 13.6 µs between slices on the vCPU side.
- Guest MMIO is ~40 % cheaper on the 5800X (DWT 31 vs 56 ns, NVIC 117 vs 182 ns), which is why it wins overall.

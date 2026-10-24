# core/qemu/patches — changes to QEMU itself

`core/qemu/apply.sh` applies every `*.diff` here to the QEMU v11.1.1 tree with `git apply`
(`--check` first; a patch that is already applied — `git apply --reverse --check` succeeds — is
skipped, so `apply.sh` stays idempotent). Keep them small: everything that can live in the machine
(`hw/arm/nexus_xr.c`) lives there.

`apply.sh` keeps a copy of every patch it applied in `<qemu>/.flightbench-patches/`; when a patch here
changes, the old copy is reversed first and the new one applied. A tree patched before these copies
existed must be restored by hand (`git -C <qemu> checkout -- <files the patch touches>`, and remove
`include/qemu/prof-hooks.h`).

## `prof-qemu.diff` — host-time profiling hooks (for `-M nexus-xr,prof=on`) and the icount notify fix
A table of callbacks, `const QemuProfHooks *qemu_prof_hooks` (new header
`include/qemu/prof-hooks.h`, defined in `util/async.c`), NULL by default. With no table every hook
site is one load and an untaken branch; the hooks only observe (they never change what the vCPU
executes or when, so a `prof=on` run flies the same as `prof=off`). The machine installs the table
when `prof=on` (`hw/arm/nexus_xr.c`, "Host-time ledger").

| hook | where | what the machine does with it |
|---|---|---|
| `slice_start` / `slice_end(cpu, ret)` | `accel/tcg/tcg-accel-ops-rr.c`, around `tcg_cpu_exec()` (after `icount_prepare_for_run`, before `icount_process_data`) | one hand-off per slice end, with its reason (budget left, `exit_request`, halted, who kicked); the time between two slices is the account `vcpu.between_slices` |
| `vcpu_wait(begin)` | `rr_wait_io_event()`, around `qemu_cond_wait_bql(halt_cond)` | `vcpu.halt_wait` (all vCPUs idle) and the main loop → vCPU kick latency |
| `kick(cpu, self)` | `qemu_cpu_kick()` (`system/cpus.c`; `cpu_exit`, `async_run_on_cpu`, interrupts from other threads all go through it) | timestamp of a kick from another thread; `self` marks an exit the vCPU asked for itself |
| `timer_exit()` | `qemu_timer_notify_cb()` on the vCPU thread (`system/cpu-timers.c`) | the slice ends because the vCPU armed a virtual timer before its budget end (`timer_rearm`) |
| `notify()` | `aio_notify()` when it writes the event notifier (`util/async.c`) | on the vCPU thread: timestamp of "vCPU wakes the main loop" |
| `poll(begin, nfds)` | both `os_host_main_loop_wait()` variants (POSIX and Win32), around `qemu_poll_ns()` (`util/main-loop.c`) | `mainloop.poll` / `mainloop.dispatch`, fds polled, and the wake latency (notify → poll returns) |
| `bql_wait(begin)` | `bql_lock_impl()` (`system/cpus.c`): with a table installed it first tries `qemu_mutex_trylock`; only a contended acquisition calls the hook | `vcpu.bql_wait` / `main.bql_wait` (count + host time blocked) |

**The icount notify fix** (the one part of this patch that changes behaviour, `util/qemu-timer.c`,
`include/qemu/timer.h`, `accel/tcg/tcg-accel-ops-icount.c`): under `-icount` the vCPU thread runs the
virtual timers itself, and before each run `icount_notify_aio_contexts()` called
`qemu_clock_notify(QEMU_CLOCK_VIRTUAL)`, which notifies every virtual timer list — for each
AioContext that is `aio_notify()`, a main-loop poll wake-up and a BQL hand-off, at every due virtual
timer (25.8 k per virtual s in the RS7 liftoff, `evidence/prof/README.md` "Review note"). The new
`qemu_clock_notify_nonempty()` notifies the main loop's own list (`main_loop_tlg`, whose callback
`qemu_timer_notify_cb()` is unchanged) as before and an AioContext's list only when it has a timer
(`timerlist_has_timers()`, per list), so an AioContext virtual timer (block throttling, coroutine
sleeps) is still woken for; replay mode (`-icount rr=`) keeps `qemu_clock_notify()`. Guest-invisible:
the FDM log is byte-identical to the build without it (`evidence/speed/README.md`, fix 1); main-loop
iterations 25.8 k → 0.4 k per virtual s, the core 21 % faster.

Not hooked (the machine counts them as `other` or does not see them): the MTTCG thread loop (the
machine only runs under `-icount`, i.e. round robin), the replay mutex, bottom halves.

## `usart-accept-input.diff` — no main-loop wake per F4 USART poll
The stock `stm32f2xx_usart` (the F405's USARTs) called `qemu_chr_fe_accept_input()` on every SR and
every DR read; that always ends in `qemu_notify_event()` → a bottom half → the main loop wakes and
takes the BQL from the vCPU. iNav's F4 UART interrupt handler reads SR/CR1/CR3 around every byte, so
the trainer's CRSF USART1 cost 935 ns per access, 15.9 k main-loop wakes per virtual s.
- `hw/char/stm32f2xx_usart.c`: `accept_input` only when RXNE really clears — a DR read with RXNE set,
  or an SR write that clears it (stock never notified there; can_receive depends on RXNE alone).
- `chardev/char-fe.c`, `include/chardev/char.h`: a chardev with the new `QEMU_CHAR_FEATURE_INPUT_PUSHED`
  ("the backend pushes its input itself, no main-loop watch waits for room") skips the wake. The
  machine's `fbwire` sets it: every USART's input is fed by the machine from virtual timers. A socket or
  mux chardev behaves as before.
Guest-invisible (FDM log byte-identical); trainer flight 7.27 → 5.64 host s per 10 virtual s.

## `nvic-icsr-cache.diff` — ICSR without a scan of all vectors
`hw/intc/armv7m_nvic.c` computed ICSR's ISRPENDING (any external IRQ pending) and RETTOBASE (at most
one active exception) by scanning every vector at every ICSR read — iNav reads ICSR in every `micros()`.
The NVIC already rescans all vectors after every change of their state (`nvic_recompute_state()`, the
central rule of that file); the two bits are computed there now (and at reset) and kept in `NVICState`
(`icsr_isrpending`, `icsr_rettobase`). Nothing outside the file writes vector state. Checked with a
build that compared the kept bits against the scans at every ICSR read: no mismatch over two boots and a
whole trainer flight. Guest-invisible; flight 5.64 → 4.72, boot from the snapshot 31 → 27.6 host s.

## `icount-io-tb.diff` — the icount rewind, memoized; one TB per I/O instruction (opt-in)
Under `-icount` a device access must be the last instruction of its TB (`can_do_io`). Found in the
middle of one, `cpu_io_recompile()` searches the TB by host PC (a g_tree), decodes its unwind data up
to that PC, rewinds, runs the access as a TB of one instruction and goes on with a new TB — at every
execution, since the cached TB stays as it was. A polling loop pays it on every pass.
- **Memo** (always on under icount, `accel/tcg/translate-all.c`): the TB search and the unwind decode
  give the same answer for a host PC until the code buffer is flushed (host code is not reused before);
  256 entries keyed by host PC and `tb_flush_count`. The restore and everything after it are unchanged.
  Guest-invisible; boot 28.8 → 21.7 host s, flight 4.72 → 4.29, RS7 9.38 → 9.14.
- **One TB per I/O instruction** (`icount_io_insn_tbs`, `include/system/cpu-timers.h`; the machine sets
  it with `-M io-tb=on`, default off): `cpu_io_recompile()` notes the instruction's PC and drops the TB;
  the translator (`accel/tcg/translator.c`) then ends a TB before a noted instruction and after it, and
  no TB jumps straight into it (`translator_use_goto_tb`), so the PC is stored as the rewind stored it.
  The notes are forgotten at `tb_flush`. Same instruction counts, interrupt points and TB starts as the
  rewind; **not** the same guest trajectory: the idle skip's signature reads `env->regs[15]`, which is
  the start of whichever TB QEMU cut (a TB cut by the icount budget right after the read leaves its
  start there, where the rewind would have stored the read's PC), so the skips land elsewhere
  (trainer `e9bf4c24732e` instead of `4f8c7114358a`, deterministic). With it: boot 21.7 → ~18,
  flight 4.29 → ~3.5 host s (`evidence/speed-inav/README.md`). Off: the note path is skipped, the set
  stays empty, translation and chaining are QEMU's.

## `win-symlink-install-tree.diff` — Windows build without the symlink privilege
QEMU's configure runs `scripts/symlink-install-tree.py` (meson postconf), which links every installed
file into `build/qemu-bundle/` so the binary runs from the build tree. On Windows without Developer
Mode `os.symlink` fails and the script stops the configure; the patch copies the file or directory
instead (`os.name == 'nt'` only — on Linux the symlinks are made exactly as before) and skips sources
that do not exist yet (build outputs, e.g. a subproject DLL). Skipped entries are not copied later:
only a re-run of the postconf (meson regenerate) picks them up, and a copy does not follow a rebuilt
file. The nexus-xr machine loads nothing from the bundle (no BIOS/ROM files), so neither matters for it.

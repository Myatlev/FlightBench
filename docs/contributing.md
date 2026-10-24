# Contributing

How the repository is laid out, what must hold before a change lands, and the conventions the code follows.
Automated agents work by the same rules.

## Map
| where | what |
|---|---|
| `core/qemu/` | the emulator core: `hw/arm/nexus_xr.c` (the machine), `stm32_soc.c.inc` (SoC and board descriptions: STM32F722 / Nexus-XR, STM32F405 / SpeedyBee F405 WING), `nexus_xr_ctl.c.inc` (control server: protocol, MSP master, CLI, first start, functions), `fb_bench_state.c.inc`, `fb_plane.c.inc`, `fb_outputs.c.inc`, `fb_prof.c.inc` (profiler), `patches/` (QEMU patches applied by `apply.sh`), `scripts/` (run.sh, flight.py, prof.py, gates.py, the evidence runners), `sessions/` (core-form benches), `tests/`, `evidence/<topic>/README.md` (what each feature was verified with) |
| `devices/` | emulator-independent device models and the bench resolver (`session.c`, `cli_norm.c`), the catalog (`catalog.c`), host tests (`make test`) |
| `fdm/` | flight dynamics (`docs/fdm.md`): `fdm.c` (the interface), `body.c`, `rotor.c`, `heli.c`, `plane.c`, `plane_scenario.c`; host tests |
| `boards/<id>/` | board data: `board.json` (pads, onboard devices, machine, MSP UART, `firmware_family`, config type), `defaults.cli` or the unified config |
| `models/<id>/` | aircraft: `aircraft.json`, firmware CLI / profile, `env/*.json` scenarios, `visual/` |
| `presets/` | app-form benches shipped with the client |
| `firmware/` | bundled firmware binaries + `FIRMWARE.md` provenance |
| `app/` | the Godot .NET client (Windows): `scripts/`, self-tests (`FB_SELFTEST`), `tools/export.ps1`, `tools/fake_core.py` |
| `docs/protocol.md` | the core ↔ client contract (launch, session-check report, messages, events) — authoritative |
| `docs/architecture.md`, `docs/ui.md`, `docs/decisions.md` | design and the reasons behind it |

## Gates: what must hold before a merge
| bench | Linux | Windows |
|---|---|---|
| RS7 liftoff (`sessions/rs7.json`) | to re-baseline (the models changed 2026-10-24) | `adcc355a6808` |
| trainer plane-launch (preset + sensors) | to re-baseline | `18c7adf95de1` |
| trainer-osd | to re-baseline | — |
| rs7-osd | to re-baseline | — |

The flight models were rewritten on 2026-10-24 (`docs/fdm.md`); every flight hash moved. The Windows baselines above
are from the rewrite's own gates run (`gates.py --new-baseline`); the Linux ones are taken on the homelab the next
time the core is built there and go into this table and `gates.py`.

The hashes are FDM-log hashes (md5, 12 hex digits) of a fixed flight plan; a guest-invisible change must not move
them, a change that moves one must say why and record the new baseline in its evidence README. One command runs
everything:

```
python3 core/qemu/scripts/gates.py --qemu <qemu-system-arm> --ports <first of five free ports> > gates.log
```

The last lines are the PASS/FAIL table; exit 0 when every gate holds. `--only host,equiv,rs7` runs a subset,
`--new-baseline <gate>` prints a hash that is expected to move instead of comparing it. The pieces, runnable on
their own: `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test`,
`make -C core/qemu/tests equiv QEMU=<binary>` (the C resolver against the Python reference, `equiv-host` without
QEMU), `python3 core/qemu/scripts/test_wiring.py`, py_compile of the scripts. App: `FB_SELFTEST=all` (see
`app/README.md`). On Windows run the gates from MSYS2 MINGW64 (`python3` there is the native one); `equiv` on
Windows reports dev benches with `~/…` firmware paths because the two sides resolve `~` to different homes
(MSYS2 `HOME` vs `USERPROFILE`) — not a gate.

Builds: `core/qemu/build.sh <qemu-src-dir>` clones QEMU v11.1.1, applies `core/qemu/patches/` and the machine
sources, builds `qemu-system-arm`; on Windows `core/qemu/tools/package-win.sh <build dir> [out dir]` collects the
exe and its DLLs into a self-contained folder (`dist/core/` by default) that `app/tools/export.ps1` bundles.

## Conventions
- The sim never masks the board: no faking firmware state (calibrations, MSP answers, pin modes). If a real board
  would not do it, the bench does not do it either.
- Nothing hidden: every new behaviour has a message or event in `docs/protocol.md` and a log line; no dev-only
  UI modes in release paths.
- Evidence with the change: a feature lands with `core/qemu/evidence/<topic>/README.md` saying what was run, the
  numbers, the hashes before and after, and what is not verified. The raw logs and CSVs live outside this repo.
- `_WIN32` guards for anything POSIX-only; the core builds under mingw.
- Small commits in English; `WIP:` commits at stable points are fine on a branch.
- Stop the cores you started by PID; never kill by image name — someone else's bench may be running.
- Licence: GPL-2.0-or-later for every file (the core is compiled into QEMU, GPL-2.0 as a whole); a contribution
  is accepted under the same terms. Source files that carry a header use `SPDX-License-Identifier: GPL-2.0-or-later`.
- Keep the FDM logs deterministic: no host time, no uninitialised memory, no `rand()` in the physics path.

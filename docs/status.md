# Status

What FlightBench v0.1 actually does, backed by an evidence file; what is described but not yet checked
end to end; and what is known not to be modelled. "Evidence" links are under `core/qemu/evidence/` unless
noted; each README describes a real run with its numbers and hashes (the raw logs and CSVs are kept outside
this repository).

## Works (verified, with evidence)

| what | evidence |
|---|---|
| RotorFlight 4.6.0 boots on the emulated Nexus-XR (STM32F722) and answers MSP on USART1 from the genuinely **official**, unmodified release hex, via one custom-defaults `serial` line — no firmware fork needed for MSP-on-a-UART | `evidence/stock-rf/README.md` |
| The RS7 (700-class helicopter) lifts off in deterministic lockstep: gyro, DShot600 bidirectional ESC, servos, CRSF receiver and the helicopter FDM closed through the IMU | `core/qemu/README.md` → Machines; `docs/contributing.md` → Gates (the Windows regression gate `adcc355a6808` after the 2026-10-24 models) |
| Outputs are gated on the pad's actual pin mode (alternate function, timer enabled, `CCxE`, `BDTR.MOE`) — a firmware that leaves an output as a driven-low GPIO gets no pulse from the bench either | `evidence/pin-mode/README.md` |
| The firmware's config is assembled as explicit, provenance-tracked layers (board/aircraft/profile/device/bench) with conflicts resolved by "later layer wins", live-checked against the real core | `evidence/config-layers/README.md`; `app/README.md` → Config layers |
| Bench state (calibrations, saved config) persists across restarts and is applied back as a delta, not a full replay | `evidence/bench-state/README.md` |
| The RS7 preset takes off **from the keyboard** on the exported release build, as shipped and with no Settings visit: key 5 arms (ch5 from the firmware's mode table), key 6 puts the bench's `throttle` switch (ch6) on and the governor spools the head, W raises the collective until the heli is airborne — driven through the app's own key handling by `FB_LIVE_CHECK=1` against the real core — **with a joypad plugged in** (an Xbox pad on the test PC: the keyboard is a layer beside it, W takes the collective from the pad's value) **and without one** (`FB_NO_JOY=1`) | `app/evidence/kb-layer-2026-10-23/README.md` (both configurations); `app/evidence/tx-model-2026-10-23/README.md` (keyboard only) |
| Inverted flight: rolled past the side with negative collective the RS7 holds and climbs at a bounded rate (full negative collective: about 17 m/s, like full positive upright); before 2026-10-23 a vertical-speed coefficient drove it to millions of m/s within seconds | `fdm/tests/test_heli.c` → claim 3 (`inverted_tests` (`make -C fdm test`); `docs/decisions.md` 2026-10-23 (evening) |
| Modes/switches are derived from the firmware's own MSP mode tables (`MODE_RANGES`, `BOXNAMES`/`BOXIDS`), including RotorFlight's AND-linked ranges | `evidence/modes/README.md` |
| A given bench + pilot script is bit-for-bit deterministic on a given platform: two runs give byte-identical FDM logs | `evidence/determinism/README.md`; `docs/contributing.md` gates table |
| The gate suite holds: on Linux the whole of it (host unit tests, `equiv` against the Python reference resolver, the baseline flights); on the Windows build the host unit tests and the RS7 flight against the Windows baseline (`equiv` there has the `~` mismatch below); on macOS (arm64 core, 2026-10-24) the whole suite: host, `equiv` 251/251, RS7 and trainer flights deterministic x2 with their own baselines | `docs/contributing.md` → Gates; `evidence/pin-mode/README.md` (Linux and Windows gate logs) |

## Not verified

| what | why it is here |
|---|---|
| Cross-platform FDM hash equivalence | Linux and Windows each produce their own byte-identical, internally-deterministic FDM log for the same bench, but the two platforms' logs differ from each other (the divergence starts in the first seconds after boot, in the firmware's sensor reads / timing rather than in the FDM integration, libm or the logging); root cause not found. `evidence/determinism/README.md` |
| The Windows-only dev-bench (`equiv`) mismatch | On Windows, dev benches with `~`-prefixed firmware paths resolve `~` differently on the two sides of the equivalence suite (MSYS2 `HOME` vs `USERPROFILE`); which side is right has not been analysed. Not a gate. `docs/contributing.md` |
| The official hex with the idle skip switched off | With the idle skip on (every shipped bench) two runs are bit-identical; with it off the FDM log of the official hex differs by a few output units from ~t = 6.5 s on, reproduced across runs, cause not found. Turning the idle skip off with this hex is untested territory. `evidence/stock-rf/README.md` (stage I) |
| The helicopter model's coefficients | Ported from an earlier SITL bench of the same class; not identified against a real RS7 flight log. `models/rs7/README.md` |

## Not modelled / known gaps

| what | notes |
|---|---|
| USB | The board's USB OTG is an unimplemented region; MSP and the CLI go over a UART (the `serial` line in the custom defaults). |
| OSD | The official target for this board has no OSD compiled in; the app's OSD window and FPV camera exist but no v0.1 preset drives them. A later release. |
| Blackbox flash | No emulated SPI flash for the firmware to write real blackbox logs to; planned later (`docs/ui.md` → Signals). Use the Signals window / CSV recording instead. |
| Compass field is fixed, not tied to the environment's location | The modelled compass's declination/inclination/intensity are device properties and do not follow the aircraft's `env` home position. `evidence/sensors/README.md` |
| Adding a new board is not documented | Planned for v0.2 (`docs/add-a-board.md`, not written yet). |
| Other firmwares and aircraft | iNav on a fixed-wing bench, Betaflight on a quad, ArduPilot, a second helicopter, a Linux desktop build — roadmap items beyond v0.1; see `README.md` → Status/roadmap. |
| macOS build | `app/tools/export.sh` makes `FlightBench.app` (universal Godot/.NET app, **arm64 core**: on an Intel Mac the app starts without a bundled core). Verified on an M-series Mac: self-tests 46/46, live-check 2 PASS from the exported bundle (RS7 lifts off from the keyboard, realtime 0.98×). Not yet: a Developer ID signature and notarization (an ad-hoc bundle runs only on the Mac that built it), a flight with a real transmitter on macOS, an Intel Mac. |

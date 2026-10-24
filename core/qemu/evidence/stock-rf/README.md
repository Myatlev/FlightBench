# Stock RotorFlight hex: does a custom-defaults `serial` line give MSP on USART1? (2026-09-30)

Question: `firmware/nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf` is a fork build
(`OPTIONS="USE_MSP_UART"`) because stock RotorFlight only answers MSP on USB, and the core has no USB
device model — the emulated Nexus-XR needs MSP over a UART. `USE_MSP_UART` only changes the *default*
function of USART1 (`io/serial.c`); custom defaults are CLI lines applied at boot
(`cliProcessCustomDefaults`), so the same result should be reachable with an unmodified official build
plus one `serial` line. **Answer: yes.** `boards/nexus-xr/NEXUS_XR.config` now carries
`serial 0 1 115200 57600 0 115200` (identifier 0 = `SERIAL_PORT_USART1`, mask 1 = `FUNCTION_MSP`,
`io/serial.h`), and the official `rotorflight_4.6.0_STM32F7X2.hex` release answers MSP on USART1 with
it, flies, and is used as the default firmware for `presets/rs7-first-flight.json`.

`rs7_stock.py` (own core, ports 31270..31274, homelab; `python3 rs7_stock.py` runs all stages):
log `stock-rf-2026-09-30.log`, csvs in this directory. Both the interrupted first pass and this clean
rerun gave the same hashes throughout.

## H. Regression: fork ELF still gives the `c5a2b9bb6c49` baseline
The new `serial` line is additive (USART1, the receiver's UART5 line is untouched); RS7 liftoff with
the fork ELF must still hash `c5a2b9bb6c49` (`core/qemu/README.md`'s baseline). Two runs:
`c5a2b9bb6c49`, `c5a2b9bb6c49` — **holds**.

## R vs N. Fork ELF vs stock hex, flight numbers
Same plan (`liftoff`), same aircraft, same board config; only the binary differs.

| | fork ELF (`STM32F7X2_OSD`) | stock hex (`STM32F7X2`) |
|---|---|---|
| firmware target reported | `rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)` | `rotorflight 4.6.0 STM32F7X2 (NEXUS_XR)` |
| FDM hash, liftoff to 45 s | `c5a2b9bb6c49` (baseline) | `1dc43fc53c87` — **different from the baseline (expected: different binary)**, but identical across its own two runs |
| lift-off | t=33.82 s | t=33.82 s |
| altitude max / at end | 3.63 m / 3.63 m | 3.61 m / 3.61 m |
| FDM attitude airborne | \|roll\| ≤ 8.38°, \|pitch\| ≤ 9.16° | \|roll\| ≤ 8.38°, \|pitch\| ≤ 9.16° |
| head speed airborne | 1793..1802 rpm | 1793..1802 rpm |
| MSP_ATTITUDE vs FDM, hands off | \|roll\| ≤ 1.0°, \|pitch\| ≤ 0.4° | \|roll\| ≤ 1.0°, \|pitch\| ≤ 0.4° |
| governor | target 1800, current 1797..1802 | target 1800, current 1793..1802 |

The stock hex flies the same plan the same way, within the noise already documented for repeated runs
of the *same* binary (`core/qemu/evidence/determinism/`, MSP sampling phase). A new FDM hash is
expected and correct: it is genuinely different machine code (no `USE_MSP_UART`, no OSD target).

## C. Configurator handshake over MSP (the actual thing being tested)
`configurator_handshake.py tcp://127.0.0.1:<msp-port> rotorflight` while the stock hex is armed and
flying, over the custom-defaults-only USART1 MSP line:

```
API_VERSION       protocol 0 api 12.9
FC_VARIANT        'RTFL'
FC_VERSION        4.6.0
BUILD_INFO        'Jun 30 2026 07:20:47 118e912 4.6.0'
BOARD_INFO        79 B
NAME              ''
rotorflight: connected (6/6 replies)
```
Exit 0 — the RotorFlight Configurator's own handshake form connects to a stock, unmodified-source
binary over the emulated USART1, using only the custom-defaults `serial` line. This is the core result.

## I. Idle-skip with a hex (no symbols) — speedup holds, but a new caveat
The idle skip (`core/qemu/README.md`, "Idle skip") detects the firmware's idle polling of DWT CYCCNT
by instruction-address signature, not by reading ELF symbols, so it was not expected to care whether
the firmware image is an ELF or a HEX. That premise holds: the stock hex does get the same kind of
speedup (idle-skip on ≈ 8.5-9.0 host s / 10 virtual s vs off ≈ 19.4-19.6, in the same range as the
documented 15.4 vs 28.1 s/10s for the fork ELF in flight).

What does **not** hold: with the fork ELF, idle-skip on vs off was previously shown bit-identical
(`core/qemu/README.md` idle-skip evidence, md5 `28450cfb7f01` both ways). With the stock hex, on and
off give *different* FDM hashes:

| | FDM hash | speed (host s / 10 virtual s) |
|---|---|---|
| idle_skip=on | `1dc43fc53c87` (matches stage N) | 8.49, 8.86 |
| idle_skip=off | `91f034b82a4b` | 19.36, 19.56 |

Reproduced identically across two independent full runs of the script (separate QEMU processes, same
two hashes both times). The logs (`i-on-fdm.csv`, `i-off-fdm.csv`) first differ at t=6.480 s, in the
governor/swash servo outputs (e.g. 1830/1342/1333/1379 vs 1829/1348/1327/1379 — a few output units),
right as the governor ramp starts; every row before that is identical. Not investigated further — this
is a genuine difference between the two binaries' code layout interacting with the CYCCNT
signature detector, not a symbol issue (the detector never reads symbols for either binary). Default
sessions and presets keep `idle_skip: true`, which is what was verified deterministic run-to-run here
and is what all the other RS7 evidence already uses; the caveat only bites if something explicitly
flips `idle_skip` off with this hex, which nothing currently does.

## What did not get touched
- `presets/rs7-fpv-osd.json` still uses the fork ELF. The `_OSD` target is the fork's own board target
  (`src/main/target/NEXUS_XR_OSD`); whether the generic stock `STM32F7X2` target has OSD/DisplayPort
  support compiled in was not checked (the sessions run here never wire an `msp_osd` device). Out of
  scope for this pass.
- `core/qemu/sessions/rs7.json` / `rs7-osd.json` / `rs7-pads.json` still point at a local dev build
  path outside the repo (`~/flightbench-spike/fw-msp/obj/main/...`) — these are developer bench
  sessions, not the app's bundled presets, and were left alone.

## Provenance
`firmware/nexus-xr/rotorflight_4.6.0_STM32F7X2.hex`: official release asset
https://github.com/rotorflight/rotorflight-firmware/releases/download/release/4.6.0/rotorflight_4.6.0_STM32F7X2.hex,
sha256 `2382bb94acc7dc2ce1e22f6b08a2a773e3cc3f30172201ddafcd7256d04c8e65`, re-downloaded and diffed
byte-identical against the bundled copy on 2026-09-30.

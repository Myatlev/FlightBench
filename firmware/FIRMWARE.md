# Bundled firmware

FlightBench ships firmware binaries so a preset flies out of the box. They are executed by the
emulated MCU, never linked into FlightBench. Each one is GPL-3.0 software of its own project; the
source for exactly this build is given below, as the licence requires.

| file | firmware | target / board | source | build |
|---|---|---|---|---|
| `nexus-xr/rotorflight_4.6.0_STM32F7X2.hex` | RotorFlight 4.6.0 | `STM32F7X2` (generic; board comes from custom defaults) | **stock, not built here**: the release asset https://github.com/rotorflight/rotorflight-firmware/releases/download/release/4.6.0/rotorflight_4.6.0_STM32F7X2.hex, sha256 `2382bb94acc7dc2ce1e22f6b08a2a773e3cc3f30172201ddafcd7256d04c8e65` (checked against the download, 2026-09-30) | **default for `presets/rs7-first-flight.json`**. MSP on USART1 comes from a custom-defaults `serial` line (`boards/nexus-xr/NEXUS_XR.config`, `serial 0 1 115200 57600 0 115200` — identifier 0 = `SERIAL_PORT_USART1`, mask 1 = `FUNCTION_MSP`, verified against `io/serial.h`), not from a firmware option: the generic stock target needs no fork build at all. Evidence: `core/qemu/evidence/stock-rf/README.md` |
| `nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf` | RotorFlight 4.6.0 | `STM32F7X2_OSD` (RadioMaster Nexus-XR) | https://github.com/Myatlev/rotorflight-firmware-osd, commit `014284a08` (`release/4.6.0-42-g014284a08`; the STM32 target code is upstream RotorFlight 4.6.0, the fork's own commits touch the SITL target) | `make TARGET=STM32F7X2_OSD OPTIONS="USE_MSP_UART"`. **Kept for `presets/rs7-fpv-osd.json` and the dev sessions `sessions/rs7.json`/`rs7-osd.json`/`rs7-pads.json`**: checked (`core/qemu/evidence/osd-stock/README.md`) that the generic stock `STM32F7X2` hex does **not** run the OSD — no `OSD:` line in `status`, no `OSD` task in `tasks`, `osd_evidence.py boot` sees 2 blank `clear` frames and nothing else — so only the `_OSD` fork target (`src/main/target/NEXUS_XR_OSD`) has `USE_OSD` compiled in; the OSD preset/sessions stay on this ELF. The board configuration is not in the binary either way: the core inserts it as custom defaults from `boards/nexus-xr/NEXUS_XR.config` |
| `speedybee-f405-wing/inav_9.1.0_SPEEDYBEEF405WING.hex` | iNav 9.1.0 | `SPEEDYBEEF405WING` (SpeedyBee F405 WING) | https://github.com/iNavFlight/inav, tag `9.1.0` (e519b69b) | **stock, not built here**: the release asset https://github.com/iNavFlight/inav/releases/download/9.1.0/inav_9.1.0_SPEEDYBEEF405WING.hex, sha256 `05740e09635f8a957928ee53b2e0cc919d1b8a1843c56339ddc15803c9c10874` (checked against the download, 2026-09-29). The board and aircraft configuration go in over the CLI on USART6 (MSP there by the target's defaults): `boards/speedybee-f405-wing/defaults.cli` + `models/trainer/inav.cli` |

## Which nexus-xr session/preset uses which file
| session / preset | firmware |
|---|---|
| `presets/rs7-first-flight.json` | `nexus-xr/rotorflight_4.6.0_STM32F7X2.hex` (stock) |
| `presets/rs7-fpv-osd.json` | `nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf` (fork, OSD) |
| `core/qemu/sessions/rs7.json`, `rs7-osd.json`, `rs7-pads.json` | `nexus-xr/rotorflight_4.6.0_STM32F7X2_OSD_msp-uart.elf` (fork; repointed from a local dev build path outside the repo, same binary, sha256-verified identical — `core/qemu/evidence/osd-stock/README.md`) |

`USE_MSP_UART` only changes the default function of USART1 (`src/main/io/serial.c`); an unmodified
official build flies the same, and the configurator/CLI path over TCP reaches MSP on a UART through a
custom-defaults `serial` line alone, no firmware option needed — confirmed for `rs7-first-flight`
(`core/qemu/evidence/stock-rf/README.md`).

ELF vs HEX and idle-skip: the idle skip (`core/qemu/README.md`, "Idle skip") detects the firmware's
idle polling by instruction-address signature at run time; it does not read ELF symbols, so ELF vs
HEX is not expected to matter by design, and with the fork ELF idle-skip on/off is bit-identical
(`core/qemu/README.md` idle-skip evidence, `md5 28450cfb7f01` both ways). With the stock hex, though,
idle-skip on vs off gave *different* FDM hashes in testing (`core/qemu/evidence/stock-rf/README.md`,
stage I) — small PWM/governor differences from ~t=6.5 s on, reproduced across two independent runs.
Root cause not found; default sessions and presets keep `idle_skip: true` (matches what was verified
deterministic run-to-run), so this does not affect normal use, but `idle_skip: false` with this hex
produces a different, equally-reproducible flight and should not be treated as equivalent until
explained.

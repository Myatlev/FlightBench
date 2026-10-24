# OSD fonts

MAX7456 `.mcm` symbol fonts (12 × 18 glyphs), copied unmodified from the firmware configurators.
The app picks one by the `osd` message's `font` field (`docs/protocol.md`; absent → RotorFlight's).
Both are licensed GPL-3.0 as part of their projects (see also `NOTICE`, which ships with the build).

| file | `font` | glyphs | source | license | sha256 |
|---|---|---|---|---|---|
| `default.mcm` | `rotorflight` (default) | 256 | Rotorflight Configurator, `resources/osd/2/default.mcm` (its default font, font version 2; a fork checkout at commit `c8dd72f`), copied 2026-09-26 | GPL-3.0 | `d3b122c5da7fe83fc10f70ee49817b9d4f4251fc2529146b8294424782b442f6` |
| `inav-default.mcm` | `inav` | 512 | INAV Configurator tag `9.1.1` (commit `4c343e38aba4ef655afd88e8339ef21d0c3c53ac`), `resources/osd/analogue/default.mcm` (git blob `5b539f189bc835fd9b90876719d56d4dcc482819`, 294 919 bytes), fetched 2026-09-30 from <https://raw.githubusercontent.com/iNavFlight/inav-configurator/9.1.1/resources/osd/analogue/default.mcm> | GPL-3.0 (<https://github.com/iNavFlight/inav-configurator/blob/9.1.1/LICENSE>) | `89e4643b168e3205a482d7ac2faaef0dfe0fa08ca0fd0c449b017be3d383d50b` |

- **RotorFlight** (Betaflight layout): glyph = the cell's byte. Codes 0xA0 and up hold the Betaflight logo tiles.
- **iNav**: glyph = `(attr & 3) << 8 | byte` (the MSP DisplayPort attr's font page = bits 8..9 of iNav's
  character code). Codes from iNav 9.1.0 `src/main/drivers/osd_symbols.h` (tag `9.1.0`, commit `e519b69b`;
  <https://github.com/iNavFlight/inav/blob/9.1.0/src/main/drivers/osd_symbols.h>): 0x60..0x7E are symbols,
  not lower case (0x63 SYM_BATT_FULL, 0x6A SYM_AMP, 0x76 SYM_ALT_M); page 1 (0x100..0x1FF) holds the logo
  (0x101..), the horizon 0x14C..0x154 (SYM_AH_H_START), its sidebars 0x131 (SYM_AH_DECORATION), the
  crosshairs 0x13A / 0x166 / 0x13B (SYM_AH_CH_LEFT / CENTER / RIGHT).

Refresh the iNav font for another configurator release: fetch `resources/osd/analogue/default.mcm` at that
tag, keep the file byte-identical (LF, no trailing newline), update the row above (tag, commit, blob, sha256)
and run the `osd` self-test (it checks 512 glyphs and the page-1 HUD glyphs).

# iNav MSP DisplayPort OSD on the SpeedyBee F405 WING (branch `inav-osd`, 2026-09-30)

The `msp_osd` device (an HD VTX's OSD side, `devices/src/msp_osd.c`) wired to UART4 of the trainer
(`core/qemu/sessions/trainer-osd.json`, the app's `inav-trainer-fpv-osd` preset), stock iNav 9.1.0.
Script: `core/qemu/scripts/inav_osd_evidence.py "C B F P"` (own core on 31280..31284, build tree
`~/flightbench-spike/qemu-osd`); gates: `gates-2026-09-30.log`.

| file | what |
|---|---|
| `inav-osd-evidence-2026-09-30.log` | C first start of the OSD bench, B boot + capture (twice: the second adds CLI `status`), F plane-launch ×2, P prof=on |
| `stream-boot.bin`, `stream-boot.canvas.txt` | every byte iNav sent to the device from power-on to past t = 30 s (`-M osd-log=`), and the canvas the core's `osd` messages had built at their last DRAW (1026): the replay test's input and expected cells |
| `stream-flight.bin` | the same for plane-launch run 1 (to t = 54 s) |
| `plane-launch-osd-prof.{json,txt}` / `plane-launch-no-osd-prof.txt` | prof=on plane-launch with / without the OSD, same build |
| `assembly-diff-vs-main.log` | every bench of the equivalence suite through `origin/main`'s resolver and this branch's |
| `gates-2026-09-30.log` | host tests, equiv, the RS7 and trainer baselines, RotorFlight with the OSD |
| `review-rotorflight-poll-2026-09-30.log` | the review: RotorFlight's answers to the poll with / without the CLI, the RS7-osd FDM log with the poll on / off and vs main |
| `gates-merge-2026-09-30.log` | the gates on `merge-osd` (origin/main + this branch, after the review) |

## What iNav 9.1.0 sends (and what it needs)
Sources: iNav 9.1.0 (tag e519b69b) `src/main/io/displayport_msp_osd.c` (DPO below),
`io/displayport_msp_osd.h`, `io/displayport_msp.h`, `msp/msp_serial.c`, `fc/fc_init.c`, `io/osd.c`,
`drivers/display.c`, `drivers/osd_symbols.h`, `fc/cli.c`; checked on the captured streams with the
device's own counters and a separate decoder.

- **Port and canvas.** `mspOsdSerialInit` opens the port with FUNCTION_MSP_OSD (io/serial.h:59, bit 25
  = 33554432) at its *peripheral* baud (DPO:423-426; `serial <id> <functions> <msp> <gps> <telemetry>
  <peripheral>`, fc/cli.c:881-910). The canvas is `osd_video_system`: HDZERO = 50 × 18, mode HD_5018
  (DPO:460-464); AUTO/PAL = 30 × 16. There is no MSP_SET_OSD_CANVAS: iNav answers the device's
  canvas request (188) with an error frame `$M!` (counted: `osd_errors=1` per contact).
- **Display choice.** fc_init.c:559-576: FrSky OSD, then the MSP OSD, then the MAX7456 — the first that
  opens wins. With the device wired, `status` says `OSD: MSP DisplayPort [50 x 18]` (cli.c:4263-4267)
  and the board's MAX7456 model sees no display writes (`max7456_display_writes=0 max7456_clears=0`).
- **The VTX has to talk first.** `output()` sends only while `vtxActive` (DPO:127-139), set by any MSP
  request arriving on the port (`processMspCommand`, DPO:503-514) and cleared 1 s after the last one
  (`checkVtxPresent`, VTX_TIMEOUT 1000 ms; "VTX sends an MSP command every 125ms or so", DPO:500).
  RotorFlight pushes without that. **Fixed in the device:** it now sends MSP_FC_VARIANT every 125 ms
  from power-on (one request in flight; none while the power is off or `link_lost` is on) and keeps
  the answer — `INAV` here (219 replies in 30 s) — which also names the font (`font` below). Without
  it (from the code; not run): the device spoke only after hearing a frame (its canvas request on a
  new contact), iNav only after hearing the device — neither starts.
- **Framing.** MSP v1 `$M>` pushes of MSP_DISPLAYPORT 182 (`mspSerialPushPort(..., MSP_V1)`, DPO:135;
  direction '>' for a result ≠ error, msp_serial.c:306); longest payload 4 + 50; never jumbo, never v2.
- **Sub-commands** (from `stream-boot.bin`: 0 ×1657, 2 ×29, 3 ×465, 4 ×102, 5 ×2 in the first capture of
  30 s with the default layout): HEARTBEAT with every OSD refresh (osd.c:5984/5999); CLEAR from
  `clearScreen` (DPO:169-185: also sends OPTIONS `[5, 0, mode]` first and blanks iNav's own buffer
  without marking it dirty) and once per `osd_msp_displayport_fullframe_interval` (10 = 1 s; DPO:257-271:
  CLEAR, then every non-blank cell marked dirty); WRITE_STRING only for dirty cells, as runs of one row
  with one page and one blink state (DPO:277-313); DRAW whenever a WRITE went out or the screen was
  cleared (DPO:315-322), at most every 4th call of the 250 Hz OSD task (62.5 Hz); OPTIONS
  `[font 0][mode 1]`. No RELEASE, no SYS (iNav has no system elements).
- **attr byte** (DPO:299-309, displayport_msp_osd.h:31-36): bits 0..1 = the font page = bits 8..9 of the
  character code (`DISPLAYPORT_MSP_ATTR_FONTPAGE_MASK` 0x3; iNav's codes run to 0x1FF, `getFontMetadata`
  claims 1024); bit 6 blink — never set, because `supportedTextAttributes` returns NONE (DPO:357-365),
  so drivers/display.c:49-70 emulates blink by blanking the element every other phase; bit 7 version
  = 0. Seen: 0x00 and 0x01 only.
- **Rows/cols.** Runs never cross a row (`endOfLine = row * COLS + screenCols`, DPO:284); iNav's buffer
  stride is 60 but only 50 columns are sent; nothing was clipped (`osd_clipped=0`).
- **Character codes = iNav's own font**, not Betaflight/RotorFlight's: `drivers/osd_symbols.h`. Text is
  upper case, digits and most punctuation at ASCII; **0x60..0x7E are symbols** (0x76 SYM_ALT_M, 0x63
  SYM_BATT_FULL, 0x6A SYM_AMP), so are 0x22 0x24 0x27 0x3F; numbers with a decimal point use their own
  glyphs (osd.c:857-858: the last integer digit + half a dot 0xA1..0xAA, the first decimal with the
  other half 0xB1..0xBA: the battery's `16.7 V` arrives as `0x63 '1' 0xA7 0xB8 0x1F`); page 1 = 0x100..0x1FF: logo 0x101.., the horizon 0x14C..0x154, its
  sidebars 0x131, the crosshairs 0x13A 0x166 0x13B, arrows.
- **Parser vs iNav.** The parser (written for RotorFlight 4.6) took iNav's stream unchanged: MSP v1 +
  jumbo, sub-commands 0..6, the double buffer (iNav relies on writes accumulating across DRAWs without
  a CLEAR — already the model), clipping, attr kept as received. Changes: OPTIONS is decoded and kept
  (`fb_msp_osd_options`, the bench line's `osd_mode=1`), error replies counted, the FC_VARIANT poll and
  its reply (`fb_msp_osd_fc_variant`, `fb_msp_osd_font`), a changed-cell count per taken frame. One
  device, no fork.

## Descriptor and machine
- `msp_osd`'s CLI lines are now per firmware family (`config_firmware`; `devices/include/fb/device.h`),
  chosen by the board's `firmware_family` in board.json (speedybee-f405-wing `inav`, nexus-xr
  `rotorflight`, absent = `rotorflight`); the C resolver and the Python reference both.
  iNav: `feature OSD`, `serial {serial} 33554432 115200 115200 0 115200`, `set osd_video_system = HDZERO`,
  and 17 `osd_layout 0 <item> <col> <row> V` lines spreading iNav's 30-column default over 50 × 18 with
  the artificial horizon, its sidebars and the crosshairs on (drawn around the canvas centre by the
  firmware: `osdCrosshairPosition`, osd.c:1194-1199), flight mode, GPS sats, heading, RSSI, GPS speed and
  altitude left/right of the horizon, home direction/distance, messages, throttle, current, battery
  voltage, mAh, on/fly time. The first start applies them (log C: `serial 3 33554432 ...`,
  `set osd_video_system = HDZERO`), `serial`/`get` read them back (log B).
- `assembly-diff-vs-main.log`: of 251 benches 248 resolve identically to `origin/main`; the 3 that
  differ are the speedybee benches wiring `msp_osd` (the new `trainer-osd`, two generated ones), where
  the device was "not modelled, left out" before. The RS7 benches (incl. `rs7-osd`) are unchanged.
- The speedybee-f405-wing machine wires `msp_osd` on USART1..UART5 (`osd_uarts` 0x1F), one per session;
  the manifest lists it on uart1..uart5, `implemented:true`. `-M osd-log=<file>` records the FC's bytes.

## Run results (`inav-osd-evidence-2026-09-30.log`)
- **C** first start with the OSD lines: script applied (137 lines), ACC + mag calibration, snapshot, in
  149 host s; `osd_mode=1 osd_fc=INAV`, 2 contacts (the reboots after the saves; the CLI-mode reboots
  do not count: iNav is silent in CLI mode).
- **B** boot from the snapshot to t = 30 s: `hello` `osd: {"kind":"msp_displayport","cols":50,"rows":18,
  "video":true,"uart":4}`; bench `osd_frames=1059 osd_writes=2010 osd_clipped=0 osd_contacts=1
  osd_polls=240 osd_variants=219 osd_errors=1 osd_options=2 osd_mode=1 osd_fc=INAV
  max7456_display_writes=0`; CLI `status`: `OSD: MSP DisplayPort [50 x 18]`.
  **`osd` messages**: the first at t = 3.43 (`clear:true`, the splash), 129 in all, `font:"inav"` on
  every one, no `link:false`. **Rate**: iNav sends a DRAW whenever it wrote a cell, and with the
  horizon on it writes every refresh (the horizon is erased and drawn again with the same glyphs):
  1059 DRAWs in 30 s (~47/s on the pad), of which ~88 % change nothing. The core no longer sends a
  delta that changes no cell (a full frame always goes), so the app gets 5.4 msgs/s on the pad (1..14
  per second: the 1 s full-frame refresh, the clock, the sidebars' scroll) and **12.1 msgs/s in the
  flight** (6..27 per second; 19.7..257.7 ms apart) — not RotorFlight's fixed 12 Hz, but what moves.
  Before that change: 46.6 msgs/s (first run, `/tmp` log not kept; numbers quoted from it: 1058
  messages, 929 without a cell).
- **The canvas on the pad** (t = 29.30, DRAW 1026; `#` = a symbol glyph, listed with its iNav name):
```
   0 |                                                  |
   1 |  ACRO                 #359#                #88   |
   2 |  ##16                                            |
   3 |                                                  |
   4 |                                                  |
   5 |                                                  |
   6 |                  #             #                 |
   7 |                  #             #                 |
   8 |                  #             #                 |
   9 |          0#      ##    ###    ##      0#         |
  10 |                  #             #                 |
  11 |                  # ########### #                 |
  12 |                  #             #                 |
  13 |                                                  |
  14 |  #DARM                 # #  0#                   |
  15 |  ##7#         THROTTLE IS NOT LOW                |
  16 |  #1###      2#                          #00:29   |
  17 |                                                  |
  0x001 SYM_RSSI (1,44)            0x008/0x009 SYM_SAT_L/R (2,2-3)   0x00c SYM_HEADING (1,23)  0x00b SYM_DEGREES (1,27)
  0x090 SYM_KMH (9,11)             0x076 SYM_ALT_M (9,40)            0x095 SYM_THR (14,2)      0x00a SYM_HOME_NEAR (14,24)
  0x010 SYM_HOME (14,26)           0x07a SYM_DIST_M (14,30)          0x06a SYM_AMP (15,5)      0x063 SYM_BATT_FULL (16,2)
  0x01f SYM_VOLT (16,6)            0x099 SYM_MAH (16,14)             0x09e SYM_ON_M (16,41)
  0x0a1 0x0b3 (15,2-3), 0x0a7 0x0b8 (16,4-5): digits with half a dot each side of the point (0.27 A, 16.7 V)
  page 1: 0x131 SYM_AH_DECORATION (rows 6..12, cols 18 and 32: the sidebars), 0x12d SYM_AH_RIGHT (9,19),
  0x12c SYM_AH_LEFT (9,31), 0x13a 0x166 0x13b the crosshairs (9,24-26), 0x154 the horizon (11,20..30)
```
  Flight mode ACRO (disarmed, no pilot client), 16 satellites, heading 359°, RSSI 88, speed 0 km/h,
  altitude 0 m, home here (SYM_HOME_NEAR, 0 m), the throttle element's `DARM` (osd.c:1239: disarmed),
  0.27 A, 16.7 V, 2 mAh, on-time 00:29, the arming blocker as a message (`THROTTLE IS NOT LOW`: nobody
  drives the receiver's channels in this stage), the horizon two rows below the crosshairs.
- **In the climb** (plane-launch run 1, t = 32 s, 12 s after the launch):
```
   1 |  ANGL                 #  0#                #88   |
   2 |  ##16                                            |
   9 |         53#      ##    ###    ##     25#         |
  10 |                  # ########### #                 |
  14 |  ## 66                 # #101#                   |
  15 |  1###              FINISHING                     |
  16 |  #1###     31#                          #00:07   |
```
  ANGLE mode, heading 0°, 53 km/h, 25 m, auto-throttle 66 % (SYM_AUTO_THR0/1), home 101 m behind
  (SYM_ARROW_DOWN at (14,24)), the launch's `FINISHING`, 31 mAh, fly time 00:07; the horizon one row
  below centre (0x14F).
- **F** plane-launch on the OSD bench, twice: FDM log **`570a8c51503e`** both runs (5400 rows,
  62 host s for 54 virtual s each). The OSD bench is its own session (the device's CLI lines and its
  UART traffic change the firmware's work), so this is its own baseline, not the trainer's.
- **P** prof=on, plane-launch to 54 s with the OSD: see "Speed" below.

## Replay tests (`devices/tests/test_msp_osd.c`)
- `test_inav_boot_stream`: `stream-boot.bin` replayed to DRAW 1026 → the device's canvas equals
  `stream-boot.canvas.txt` (the canvas the core's `osd` messages built: the whole machine path,
  900 cells); a clean v1 stream (no bad checksum, jumbo, unknown sub-command, clip, RELEASE, SYS),
  OPTIONS font 0 mode 1, attrs 0/1 only with page-1 cells present, FC variant INAV; and cells checked
  by hand against the layout and iNav's symbol table: ACRO at (2,1), SYM_SAT_L/R + 16, SYM_HEADING ..
  SYM_DEGREES, SYM_RSSI, SYM_KMH, SYM_ALT_M, SYM_BATT_FULL .. SYM_VOLT, `THROTTLE IS NOT LOW` on row 15,
  the crosshairs 0x13A 0x166 0x13B at (24..26, 9), 14 sidebar cells 0x131 at columns 18/32 rows 6..12,
  ≥ 9 horizon cells 0x14C..0x154.
- `test_inav_flight_stream`: `stream-flight.bin`, every DRAW taken as the core does (> 1000): clean,
  attrs 0/1, ANGL in > 100 frames, the horizon crossing column 22 on ≥ 3 different rows.
- `test_poll`: the request bytes, 125 ms grid from power-on, one in flight, INAV/RTFL → font, `$M!`
  counted, OPTIONS kept, all forgotten on contact loss, none under `link_lost`.

## Gates (`gates-2026-09-30.log`)
Host tests all pass (`test_msp_osd` with the replay tests, `test_session` 216, `test_wiring.py`, the
rest unchanged); `equiv` (QEMU session-check vs the Python reference) **251/251** (the new
`trainer-osd` session is the 251st); assembly diff vs `origin/main` 248/251 identical (above).
- RS7 liftoff (`speed_evidence.sh "S E"`, RUNS=2): **`c5a2b9bb6c49` ×2** (8.42 / 8.79 host s per 10 virtual s).
- Trainer (`plane_evidence.py "C L P"`, no OSD, first start + plane-launch): **`51d4a9212465` ×2**.
- RotorFlight with the OSD (`osd_evidence.sh "B C"`, the RS7-osd bench on this build): the device's poll
  is answered (`osd_fc=RTFL`, 74 answers to 150 requests in 18.8 s — the CLI entered at t ≈ 12 stops
  all MSP on the FC; see the review below), MSP_OSD_CANVAS reads back 50 × 18, the
  canvas-driven save + reboot as before (full frames at 1.39, 1.56, 2.92), lift-off at t = 33.82 s,
  `osd` 11.5 msgs/s armed (12 Hz minus the DRAWs that changed nothing, which are not sent any more;
  before: 12.00 Hz with empty deltas). The RS7-osd bench is not a pinned baseline; its flight now
  carries the poll's MSP traffic.

## Speed
plane-launch to 54 virtual s on the trainer, same build, one after the other on a host with load ~3.3
(another agent's cores): noisy.

| | host s for 54 virtual s (prof off) | prof headline, host s / 10 virtual s | ping-watched in flight |
|---|---|---|---|
| trainer, no OSD (gates) | 61, 59 | 11.72 | 6.29 |
| trainer-osd (F, P) | 62, 62 | 12.85 (first run: 12.06) | 6.98 (first run: 6.54) |

The OSD's own accounts are small (prof, with the OSD): `UART4` MMIO 0.21 % of wall, `osd.fc_tx` (the
device parsing 2140 bytes per virtual s) 0.07 %, `ctl.osd_json` 0.01 % (466 messages: 9/s after the
empty-delta skip; 2108 in the first run, 0.02 %), `ctl.tx` 0.32 vs 0.24 %; the MAX7456's SPI2 drops
from 0.02 % to 0. The rest of the difference is guest execution (not split out by the ledger): iNav
renders the OSD into its MSP buffer and serves the UART4 interrupts, and answers the 8 requests/s —
roughly +3..10 % host time, within the noise of these runs.

## App note: what the app needs for iNav's OSD
The app's OSD panel today (`app/scripts/OsdPanel.cs`, `OsdFont.cs`) draws codes 0x00..0xFF from
RotorFlight's font (`app/assets/osd/default.mcm`, 256 glyphs) and ignores the page ("only page 0
exists"). For iNav's stream that is wrong in two ways:
1. **Wrong glyph set.** iNav's codes index iNav's font (`drivers/osd_symbols.h`): the same byte is a
   different symbol (iNav 0x76 SYM_ALT_M, 0x63 SYM_BATT_FULL, 0x90 SYM_KMH; in RotorFlight's font
   0x72..0x74 are the crosshair, 0x96 a battery). Text (upper case, digits) happens to coincide.
2. **Page 1 lost.** iNav uses 9-bit codes; page 1 carries the whole HUD — the horizon 0x14C..0x154,
   sidebars 0x131, crosshairs 0x13A/0x166/0x13B, arrows, logo. Drawn from page 0 they come out as
   0x54 'T', 0x31 '1', 0x3A ':' ... (the horizon becomes a row of `T`s).

What it needs:
- **Pick the font by the `osd` message's `font`** (new: `"inav"` | `"rotorflight"`, absent until the
  firmware is known — keep the previous/default then). Documented in `docs/protocol.md` `osd`.
- **An iNav atlas of 512 glyphs**: iNav's analog font is the same MCM format the app already parses,
  12 × 18, with 512 characters — `inav-configurator` 9.1.1 `resources/osd/analogue/default.mcm`
  (GPL-3.0; the `MAX7456` header + 512 × 64 lines). `OsdFont` needs `Glyphs = 512` for it (atlas 16 × 32
  cells) and must keep reading 256 for RotorFlight's. For a sharper HD picture the configurator also
  ships per-code PNGs, `resources/osd/digital/default/{12x18,24x36,36x54}/NNN.png` (ranges as
  `NNN-MMM.png`/`NNN_MMM.png`): an optional second step.
- **Glyph index** for `font:"inav"`: `(attr & 3) << 8 | byte` (the page in bits 0..1). For RotorFlight
  keep the byte (its pages are 0 unless `displayport_msp_attrs` is set).
- Nothing else changes: iNav never sets blink (0x40), so the blink logic stays idle; `NO SIGNAL` is
  upper case and digits, the same codes in both fonts; the canvas is 50 × 18 for both.
- **Skipped `frame` numbers need nothing** (checked in the review): `OsdBuffer.Apply` never reads
  `frame`, applies each message's `clear`/cells as they come, and has no rate or staleness timer —
  "no signal" comes only from `link:false`. Fewer messages = fewer updates of an unchanged canvas.

## Caveats
- **One canvas**: the device's canvas stays 50 × 18. iNav sets its own grid from `osd_video_system`
  (not negotiated): with `HDZERO` (the device's line) they match; a profile that sets AVATAR (53 × 20)
  or DJIWTF (60 × 22) would be clipped (counted), AUTO/PAL draws 30 × 16 in the corner. `osd_mode`
  on the bench line shows what iNav announced. DJI-compat modes (`BF43COMPAT`/`BFHDCOMPAT`, iNav's
  `getDJICharacter` mapping to Betaflight codes) were not exercised.
- **The layout is the bench's choice**: the device's `osd_layout` lines (17 items) come after the
  session profiles in the script, so they override a pasted profile's positions of those items (as
  the RotorFlight lines do).
- **Rate change for RotorFlight too**: a DRAW that changes nothing is not sent (both firmwares); a
  client that counted messages as frames sees gaps in `frame`.
- ~~The poll is on for RotorFlight as well~~ — changed in the review: off on the nexus-xr (below).
- **The OSD bench's flight hash** `570a8c51503e` is its own; it differs from the trainer's
  `51d4a9212465` because the firmware does more (the OSD task's rendering, UART4), which moves the
  scheduler under icount — expected, not a model change in the flight.
- **Streams**: `stream-boot.bin` runs on past the canvas file's DRAW (the core keeps going until the
  CLI is entered at a host-dependent moment), so its length varies by run; the replay test stops at
  the canvas file's DRAW number. Both are rewritten together by stage B.
- Not seen from iNav and so not re-verified against it: RELEASE, SYS, blink, jumbo frames, MSP v2
  DisplayPort (the parser's handling of those is the RotorFlight-era code and its unit tests).

## Review (2026-09-30, second pair of eyes)
Verdict: merged after one behaviour change (the poll) and one gap (the manifest); the rest holds.
- **RotorFlight's 74/150 was the CLI, not RotorFlight** (`review-rotorflight-poll-2026-09-30.log`).
  Decoding the FC→device stream (`-M osd-log=`): without the CLI RotorFlight answers 132 of 155
  requests to t = 19 s, and in steady state **8 replies per 12 DRAWs = every request**. The 23
  unanswered ones fall in the boot and the canvas save + reboot (port not open yet). Stage B
  (`osd_evidence.sh B`) enters the CLI at t ≈ 12 over the MSP UART; RotorFlight's serial task then
  runs `cliProcess()` only (fc/tasks.c `taskHandleSerial`), so no MSP port is served and the OSD stops
  too (117 DRAWs vs 203) — reproduced exactly (74 answers, 117 DRAWs). No rate limit
  (`mspSerialProcess` handles one command per port per pass at 100 Hz), no collision with the core's
  MSP master (that is USART1, the OSD USART4, nothing else on it). (RotorFlight can drop a reply when
  its TX buffer is busy — `mspSerialSendFrame` refuses a frame that does not fit — but that did not happen here.)
- **The poll is guest-visible on RotorFlight**: RS7-osd FDM log to 45 s `1e950839aa87` ×2 with it,
  `233f9d6f4ec7` ×2 without (first difference at row 436, t ≈ 8.7 s; the flight equivalent: lift-off
  33.82 s both, attitudes within 0.04°). RotorFlight needs none of it, so **the poll is now per board**:
  `FbBoard.osd_poll` (on for the speedybee-f405-wing = iNav, off for the nexus-xr = RotorFlight),
  `-M osd-poll=on|off` to override, `fb_msp_osd_set_poll()` in the device (test in `test_poll`).
  RS7-osd on this branch = main's build, byte-identical (`233f9d6f4ec7`). `font` for RotorFlight then
  comes from the control server's own FC_VARIANT (as before the device knew it).
- **Manifest**: the boards did not carry `firmware_family` (the app could see `config_firmware` but not
  which family applies). Added to both composers (manifest.py and session.c, byte-identical: equiv
  251/251), `docs/protocol.md`, tests.
- **Verified as the author said**: the 17 `osd_layout` lines are valid for iNav 9.1.0 (`cliOsdLayout`:
  `<layout> <item> <col 0..63> <row 0..63> V|H`; item numbers = `osd_items_e`), and they move all 10
  items iNav's default layout shows plus 7 more — ours, documented in the board README;
  `displayPortType` "MSP DisplayPort" → `status` `OSD: MSP DisplayPort [50 x 18]`; the MAX7456 gets 0
  writes once the script has configured the MSP OSD (a bench's first boot on iNav's defaults, before
  the script, still draws on it: 377 writes in stage C, as a real board would); the replay tests
  (900 cells) and every device test pass under ASan/UBSan; `osd-log=` uses `fopen("wb")` only, so it
  works on Windows too; the app tolerates skipped `frame` numbers (above).
- **History**: the two `WIP:` commits re-labelled with their content (no squash needed).
- **Gates on `merge-osd`** (`gates-merge-2026-09-30.log`): host tests, equiv 251/251, RS7
  `c5a2b9bb6c49` ×2, RS7-osd `233f9d6f4ec7` ×2 (its baseline, = main) with lift-off 33.82 s and 11.51
  osd msgs/s armed, trainer `51d4a9212465` ×2, trainer-osd `570a8c51503e` ×2 (its baseline).

# devices/ — emulator-independent device and protocol models (C)

Plain C11, no emulator types in the API. Inputs are bytes, chip-select edges, physical quantities
and time; outputs are bytes, register values and line states. The emulator (the QEMU machine; any
other host could bind the same API) owns buses, pins, clocks and interrupts.

```bash
make        # build/libfbdevices.a
make test   # host-side unit tests (gcc, no hardware)
```

## Models
| model | header | status |
|---|---|---|
| ICM-42688-P IMU on SPI | `include/fb/icm42688p.h` | register model complete for what the RotorFlight driver uses (bank select, soft reset, WHO_AM_I, PWR_MGMT0, FSR/ODR, INT config, DATA_RDY clear-on-read, big-endian data regs); FIFO/APEX/self-test not modelled. Test replays the driver's init sequence. |
| DPS310 / SPL06-001 barometer on I2C | `include/fb/dps310.h` | ID, soft reset, COEF block with synthetic coefficients that make the driver's compensation invert exactly, PRS/TMP_CFG rates and oversampling, MEAS_CFG continuous and one-shot modes (one-shot: `fb_dps310_oneshot_us()` = the datasheet conversion time, then one sample) + RDY flags, 24-bit data, byte-level slave API for an I2C controller (`_i2c_start/_write_byte/_read_byte`), the datasheet's pressure noise per oversampling (16x: 0.35 Pa RMS, fixed-seed PRNG, `_set_noise_scale`), fault `freeze` (the frozen result registers survive SOFT_RST and a power cycle). Test replays the driver's configure/read sequence and recomputes pressure/temperature, byte-level transactions, RDY per result, the end of the register file (read/write past 0x28 NACKs), noise statistics, a golden raw-register check after 100 noisy samples (the noise is reproducible), freeze incl. across resets, one-shot. The QEMU machine puts it on I2C3 (`core/qemu/README.md`). |
| SPL06-001 barometer on I2C (SpeedyBee F405 WING, iNav) | `include/fb/spl06.h` | its own device (`spl06`), sharing the DPS310's register layout, scale factors, conversion times and coefficient packing through `src/baro_common.c`. Virtual-time chip clock (`_tick`, `_next_due_ns`): SENSOR_RDY 12 ms / COEF_RDY 40 ms after power-on and SOFT_RST, command mode (the result and PRS_RDY/TMP_RDY one conversion time after the MEAS_CTRL write, then standby), background mode at PM_RATE/TMP_RATE, RDY cleared by reading B0, INT_STS clear-on-read; nine non-zero synthetic coefficients, the full compensation (c00..c30, c01/c11/c21 temperature terms) inverted by Newton, so iNav's float formula returns the set pressure within its integer-Pa rounding (worst 0.59 Pa over -500..9000 m at 1x..128x) and the temperature within 0.005 degC; seeded pressure noise; faults `freeze`, `offset`, `detached`. `include/fb/atmosphere.h`: the ISA formulas the Nexus-XR machine feeds its DPS310 with, for the SPL06 path too. Test replays iNav 9.1.0's `barometer_spl06.c` detect/calibration/configure and `baroUpdate()` in virtual time. Not modelled: FIFO, INT pin, SPI, a missing P_SHIFT/T_SHIFT at > 8x. |
| QMC5883L magnetometer on I2C (the GPS module's compass; SpeedyBee F405 WING, iNav) | `include/fb/qmc5883l.h` | register level per the datasheet and iNav 9.1.0's `compass_qmc5883l.c`: ID 0xFF at 0x0D, CONTROL 1 (mode, ODR 10/50/100/200 Hz, range 2/8 G, OSR), CONTROL 2 (INT_ENB, ROL_PNT, SOFT_RST), SET/RESET, continuous mode on the chip's own clock (`_tick`, `_next_due_ns`), DRDY cleared by a data read, DOR for skipped samples, OVL + saturation, 16-bit XYZ LSB first at 12000 / 3000 LSB/G, TOUT at 100 LSB/degC; the field in the chip's axes (`_set_field`, gauss) + noise (sigma mG, fixed-seed PRNG, the datasheet's 2 mG resolution by default); faults `freeze`, `offset`, `noise`, `detached`. Field helpers: the earth field in NED from declination/inclination/intensity (`fb_mag_field_ned`, defaults for Yerevan from WMM2025: D 6.52°, I 59.45°, F 49.67 µT; iNav's own declination table gives 6.52° there), NED → body FRD by the attitude, FRD → the chip for a mount = the inverse of iNav's `applySensorAlignment` (`fb_mag_body_to_chip`; the firmware's board frame is (x, −y, −z) of FRD). Test replays the driver's detect/init/read, the rotation (heading 0/90/180/270 → the chip's XY with the default CW270FLIP mount, every mount against iNav's alignment code), the faults. |
| LiPo battery (the FC's VBAT / current sensor) | `include/fb/battery.h` | pack voltage = cells × OCV(SoC) − I × R (a LiPo curve 4.20 V full … 3.30 V empty per cell, piecewise linear), SoC integrates the current (mAh used), R default 0 (no sag); what the FC's sensors see (`_sense_v`, `_sense_a`) with the faults `voltage` (a fixed reading) and `disconnect` (0 V, 0 A); the pack itself is unaffected by both. Props: cells 4, capacity 4200 mAh, start SoC 1, resistance 0, current_scale 1 (× the ESC's current), fc_current_a 0.3. The battery feeds nothing back (the ESC keeps its own supply voltage). Test: the curve, the integration, sag, faults. |
| u-blox GNSS receiver (M10 / M8 class, UBX) on UART | `include/fb/ublox.h` | everything iNav 9.1.0's `io/gps_ublox.c` does, both paths: `$PUBX,41` autobaud (heard only at the receiver's own baud: `_set_fc_baud` tells the model the FC's USART baud), MON-VER (hw 000A0000 / 00080000, PROTVER 34.10 / 18.00), MON-GNSS, CFG-VALSET (M10) or CFG-NAV5/MSG/RATE/SBAS/GNSS (M8) with ACK-ACK/ACK-NAK, then NAV-PVT + NAV-SIG (M10) / NAV-SAT (M8) at the configured rate, other NAV messages on request; lat/lon from FDM NED around home on WGS-84 radii, fix after `fix_delay_s`, UTC and iTOW from `epoch_unix` + virtual time, Gauss-Markov position noise from `seed`, per-epoch UART capacity at the current baud; faults `no_fix`, `spoof`, `drift`, `silence`, `glitch`. Test scripts an iNav-like master through the whole handshake of both generations and decodes as `gpsParseFrameUBLOX()` does. No NMEA output. |

| MAX7456 / AT7456E analog OSD on SPI | `include/fb/max7456.h` | 16-bit address/data words per CS frame, the register file with reset values, VM0 software reset and DMM clear display done at once (the bits read back 0), display memory 512 cells in 16- and 8-bit mode with auto-increment up to the 0xFF escape, character memory: CMM read/write NVR through the 64-byte CMDI/CMDO shadow (512 characters, AT7456E), STAT = loss of sync (no video). No video timing, no overlay. Test replays the register sequences of iNav 9.1.0's `drivers/max7456.c` (init, NVM font read and write, screen update in 16- and 8-bit modes, clear display). The QEMU machine puts it on SPI2 of speedybee-f405-wing (CS B12): iNav's driver polls DMM until clear is done and would spin on an empty bus. |
| MSP DisplayPort OSD (HD VTX side: HDZero/DJI/Walksnail class) on UART | `include/fb/msp_osd.h` | parses the firmware's MSP v1 `$M>` MSP_DISPLAYPORT (182) pushes: heartbeat, release, clear, write string `[row][col][attr][chars]` (clipped at the canvas, counted), draw (double buffer), options/sys counted; 18 × 50 canvas announced once per contact with `$M<` MSP_SET_OSD_CANVAS (188) `[50, 18]`; `take_frame()` once per DRAW with the changed cells, full on a new contact (≥ 0.25 s of silence), after RELEASE and after `link_lost`; attr bytes kept as sent (bits 0..5 font page, 6 blink, 7 version: `docs/protocol.md` `osd`). Test replays the byte stream RotorFlight 4.6's `io/displayport_msp.c` sends. The QEMU machine puts it on uart3..6 (`-M osd=`). |
| CRSF receiver (ELRS/Crossfire) on UART | `include/fb/crsf.h` | emits RC_CHANNELS_PACKED (16 × 11 bit, legacy scale) at the packet rate and LINK_STATISTICS periodically; link-lost silence; parses telemetry from the FC (any type, last payload + counters) with battery/attitude decoders; CRC8 DVB-S2. Test unpacks frames the way rx/crsf.c does. |

| DShot codec + bidir eRPM reply | `include/fb/dshot.h` | packet encode/decode (both checksum polarities), decode straight from 16 timer compare values, throttle scaling; eRPM → 12-bit period code → GCR 21-bit edge stream → edge timestamps; verbatim port of the firmware's `decodeTelemetryPacket` for tests. Exhaustive encode check against `prepareDshotPacket`. |
| ESC + motor | `include/fb/esc.h` | first-order motor (kv, poles, tau, load droop), current/mAh/temp, KISS/BLHeli32 10-byte telemetry frame on request (crc8 poly 0x07); signal loss: no DShot frame for `signal_timeout_ms` (prop, default 100, 0 = never) → throttle 0. |

Emulator helpers (no device, same library): `include/fb/json.h` (JSON numbers for the catalog and the
control server: integers as integers, `%.9g`, times with 4 decimals, NaN/Inf → null; and a non-copying
reader: flat node array, object lookup/iteration, string decoding, errors with line/column;
`tests/test_json.c`), `include/fb/fw_image.h` (ELF/Intel HEX/raw firmware → flash image with every
header bound checked, custom-defaults area from the image's pointers; `tests/test_fw_image.c`) and
`include/fb/session.h` (the bench resolver of `-M <machine>,session=`: bench JSON + data root → the
board's assembly, machine options, custom-defaults blob or CLI script, manifest; a port of
`core/qemu/scripts/session.py`/`wiring.py`/`mkdefaults.sh`/`manifest.py`, tested by
`core/qemu/tests/test_session.c` and `test_session_equiv.sh` (`make -C core/qemu/tests equiv-host`
runs it through the host tool); `make tools` → `build/fb_session` prints what `session-check=on`
prints).

The resolver knows the emulated boards from one table (`BOARDS` in `src/session.c`, `MACHINES` in
`wiring.py`): per board its data directory, the (device, port) → pads its QEMU machine models, the
servo outputs in `-M servos` bit order, the aircraft FDM types it runs, and whether it is `legacy`
(the old slot form and "no wiring = the machine's defaults"). `boards/<id>/board.json` adds the
board's `machine` (the QEMU machine type, `fb_session_machine()`), `msp_uart` (the MSP/CLI host pad,
`fb_session_msp_uart()`), and `config`:
- nexus-xr (RotorFlight): a unified config (`NEXUS_XR.config`) gives the pads; the bench's
  `board_config` + profiles + the wired devices' `config` lines become the custom-defaults blob
  (`fb_session_defaults()`); manifest `config: "unified"`.
- speedybee-f405-wing (iNav 9.1.0, `config: "cli"`): the pads come from board.json alone; a pad
  of kind `output` takes a motor or a servo port. There is no blob: `fb_session_config_cli()` is one
  CLI script the machine sends over its MSP UART (uart6) — the base script (`board_config`, default
  board.json `defaults_cli`), the aircraft's `firmware_cli`, the profiles, `profile_text`, the wired
  devices' lines; stripped, blank and `#` lines dropped, `save`/`exit` dropped with a note (the
  machine saves at the end), `defaults`/`reboot`/`dfu`/`bl`/`bootloader`/`msc` an error. The old
  slot form is rejected; a bench without wiring gets the onboard devices only (no receiver, ESC or
  servos) and explicit device options; no FDM runs on this board yet (a warning). The
  session-check report has `board_machine:`, `machine=` on the launch line and the script as
  `config_cli: N lines` + `config_cli| <line>` (docs/protocol.md "Launching the core").

The generic `gps` and `compass` entries of the catalog (no model) stay for the benches that name them;
the models are `ublox` and `qmc5883l`.

## Descriptors and faults
Every model announces itself with a descriptor (`include/fb/device.h`, `fb_<dev>_desc()`): id,
category, its ports (id, the pad kind it plugs into, direction from the device's side, optional /
bundled; the old single `kind` = the first port's), props (wiring options), faults (with numeric
params), the control-server signals it feeds and, optionally, `config`: firmware CLI lines the core
appends to the custom defaults when the device is wired (`{serial}` = the UART pad's serial index;
msp_osd: MSP on the port, `displayport_msp_serial`, an element layout). The ESC has three ports (`signal` motor in, `tlm`
uart out, `rpm` capture out), the IMU `spi` plus a bundled `int`. Devices without a model file (servo output; the generic GPS and compass; the onboard SPI flash; the SpeedyBee F405 WING's MAX7456 OSD, whose model has no descriptor of its own, and SD card)
are described in `src/catalog.c`, which also holds the catalog (`fb_device_catalog()`) and its JSON.
`make tools` builds `build/fb_manifest`, which prints the catalog: the device part of the core manifest
(`core/qemu/scripts/manifest.py`, `docs/protocol.md` `manifest`). A new model adds its descriptor and
a line in `fb_device_catalog()`; the emulator checks `fault` commands against the same descriptors.

| model | faults (`fb_<dev>_set_fault(dev, id, on, params)`, kept across `_reset`) |
|---|---|
| crsf | `link_lost` (silent), `freeze` (repeats the RC frame of the fault moment, link statistics live), `lq` (LINK_STATISTICS with params lq, rssi_dbm, snr_db; NaN = keep) |
| icm42688p | `freeze` (data registers stop updating, DATA_RDY/INT1 continue) |
| esc | `telemetry_lost` (`fb_esc_telemetry_ok()` false: no bidir reply, no KISS frame; the motor runs on) |
| spl06 | `freeze` (as dps310), `offset` (param `offset_pa`, required: added to the measured pressure), `detached` (every address phase NACKs, reads 0xFF) |
| qmc5883l | `freeze` (the data registers stop, DRDY keeps coming), `offset` (params `x_mg`, `y_mg`, `z_mg`, each default 0: hard iron in the chip frame), `noise` (param `sigma_mg`, required: instead of the configured noise), `detached` (the address NACKs) |
| battery | `voltage` (param `volts` 0..60, required: the FC's voltage sensor reads it), `disconnect` (the sense leads are off: 0 V, 0 A) |
| ublox | `no_fix` (fix 0, 2 sats, messages keep coming), `spoof` (`north_m`, `east_m`, `alt_m`), `drift` (`rate_m` per √s random walk; off = back at once), `silence` (sends and hears nothing), `glitch` (`jump_m` north for one epoch, 0 = one frame with a broken checksum; turns itself off) |
| msp_osd | `link_lost` (the device ignores the FC and sends nothing: no frames; off → canvas announced again, next frame full) |

`tests/test_faults.c` covers the catalog and each fault.

## How the F405 WING machine wires spl06, qmc5883l, ublox, battery (phase 4a)
`core/qemu/hw/arm/nexus_xr.c`; evidence `core/qemu/evidence/sensors/README.md`.
- **spl06 on I2C1 (0x76)**, the board's onboard baro: the v1 controller's slave callbacks call
  `fb_spl06_i2c_start` (its return = the address ACK: `detached` NACKs), `_write_byte`,
  `_read_byte`; one virtual-clock timer: at every address phase, every STOP and when it fires the
  chip gets the environment (`fb_spl06_set_environment`, the ISA formulas the Nexus-XR's DPS310
  gets too: field elevation + FDM altitude) and is ticked to now, then the timer goes to
  `fb_spl06_next_due_ns()`. iNav programs command mode at 8x: a conversion per 16 ms. Noise from
  the wiring's `noise_scale` (seed 1); a power cycle resets the chip.
- **qmc5883l on I2C1 (0x0D)**, wired with the GPS module (`-M mag=on`): no timer; at each address
  phase the earth field (props `declination_deg`, `inclination_deg`, `intensity_ut`) is turned into
  the body by the FDM's attitude and into the chip by its `mount` (the firmware's `align_mag`,
  default CW270FLIP), handed over, and the chip's clock ticked to now.
- **ublox on a USART** (`-M gps=uartN`, the target's GPS port is USART3): an in-process wire; FC TX
  bytes → `fb_ublox_rx_byte()`; the receiver's bytes enter the USART one per byte time at its baud
  when RDR is free, one deadline per navigation epoch otherwise; every BRR write →
  `fb_ublox_set_fc_baud(pclk / brr)`; the FDM's NED position/velocity every physics tick
  (`fb_ublox_set_motion`) around home (40.1792, 44.4991, the baro's field elevation; `env.home`
  moves it); a power cycle → `fb_ublox_reset()`, an MCU reset leaves it at the baud iNav set.
  The descriptor's `config` lines (`feature GPS`, `serial {serial} +1`, `gps_provider = UBLOX`,
  `gps_ublox_nav_hz = 10`) go into the CLI script; the board script no longer has `feature -GPS`.
- **battery on the BAT pad** (`-M battery=on`): stepped at 1 kHz with `current_scale × the ESC's
  current + fc_current_a` (0 while the bench is powered off); ADC1's VBAT channel reads
  `sense_v / 11` (iNav's default `vbat_scale` 1100), CURRENT `sense_a × 19.5 mV` (the target's
  `current_meter_scale` 195), through DMA2 stream 0 into the firmware's buffer.
- Signals: `baro.pressure_pa`/`baro.temp_c`; `mag.x_g/y_g/z_g` (the chip-frame field it was given);
  `gps.*` from `fb_ublox_solution()`; `battery.voltage_v/current_a/mah_used/soc`.

## Conventions
- One `fb_<chip>_t` per instance; `_new/_free/_reset`; bus callbacks are plain functions
  (`_cs`, `_xfer` for SPI; `_start/_write/_read/_stop` for I2C; `_rx_byte/_tx_pop` for UART devices).
- Physical inputs in the chip's own frame and SI-ish units (deg/s, g, degC, Pa). Mounting
  orientation is applied by the board/wiring layer.
- Time: the emulator asks the model for its rate (`_odr_hz`) and calls `_sample()` on that schedule
  in virtual time; the model says whether an interrupt line should pulse.
- Every model ships with a test in `tests/` that exercises the exact register sequence of the
  firmware driver it is meant for.

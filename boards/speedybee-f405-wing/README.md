# SpeedyBee F405 WING (STM32F405, iNav 9.1.0)

Stock iNav, target `SPEEDYBEEF405WING` (release asset `inav_9.1.0_SPEEDYBEEF405WING.hex`). Background
and the choice of configuration path: `docs/spike-inav-plane-2026-09-30.md` (A1-A3).

- `board.json` — the whole board: iNav has no unified config, so the pads come from here alone
  (`id`, `kind`, `pins`, `group`, `bus`, `timer`, `serial` = the iNav serial identifier, `label`,
  `flags`, `note`), plus the onboard devices (`pad`, `device`, `detachable`, `part`), what is not a
  pad (`unmapped`: LED strip, RSSI/airspeed ADC inputs, PINIO, status LEDs) and the board keys the resolver reads:
  `machine` (the QEMU machine type), `mcu`, `config: "cli"` (configured by a CLI script, no
  custom-defaults blob), `firmware_family: "inav"` (which of a device descriptor's
  `config_firmware` line sets the wired devices bring), `defaults_cli` (the base script), `msp_uart` (`uart6`: the MSP/CLI host
  socket) and `beeper` (`C15:inv`). Every value cites its source (`sources`, file:line in iNav 9.1.0).
  The outputs are pads of kind `output` (`output1`..`output11`, printed S1..S11): iNav's
  `TIM_USE_OUTPUT_AUTO` outputs take a motor or a servo. UART pad labels are `null` (not sourced).
  `batt1` (label BAT, kind `power`: VBAT C00, CURR C01) is the battery input (phase 4a).
- `defaults.cli` — the base CLI script: comments only today (until phase 4a it had `feature -GPS`,
  a guard while no GPS was modelled: the wired `ublox`'s lines turn GPS on now, and without a GPS
  the target's defaults leave it off). The resolver sends it first, then the aircraft's
  `firmware_cli`, the bench profiles and the wired devices' lines, as one script (`config_cli`) the
  machine types into the CLI over USART6 and saves. Aircraft settings (platform type, mixer, modes)
  do not belong here.

Modelled (phase 1 and 4a, `core/qemu/scripts/wiring.py` MACHINES, `devices/src/session.c` BOARDS):
- the ICM-42688-P on SPI1 (`gyro1`, CS A04; polled, no INT pin), detachable;
- I2C1 (`i2c1`, the STM32F4's v1 I2C controller, `core/qemu/hw/arm/stm32_i2c_v1.c.inc`) with the
  onboard SPL06-001 baro at 0x76 (detachable; iNav's AUTO finds it: `BARO=SPL06`) and, when wired,
  the GPS module's QMC5883L compass at 0x0D (`MAG=QMC5883`; iNav's `align_mag` DEFAULT = CW270FLIP,
  the device's `mount` prop by default);
- a u-blox GPS (`ublox`) on `uart1`..`uart5` (`uart3` = the target's GPS port, serial 2; its CLI lines
  come with it);
- the battery (`battery`) on `batt1`: ADC1 with DMA2 stream 0 as iNav's `adc_stm32f4xx.c` programs
  it; VBAT through an 11:1 divider (no `VBAT_SCALE_DEFAULT` in the target: iNav's 1100), the current
  sensor at 19.5 mV/A (`CURRENT_METER_SCALE` 195): `status` shows `Voltage: 16.79V (4S battery - OK)`
  for the default full 4S pack;
- a CRSF receiver on `uart1`..`uart5` (USART1 is the target's receiver port);
- the ESC on S1 (`output1`), servos on S1..S11;
- the MAX7456 on SPI2 (`osd1`, CS B12, not detachable): registers, display and character memory, no
  video (`devices/include/fb/max7456.h`). It is there because iNav needs it to boot: `max7456Init`
  and the OSD task poll DMM until the clear-display bit reads 0, and an empty SPI bus reads 0xFF, so
  without the chip the firmware spins in `drivers/max7456.c` forever. `status` shows `OSD: MAX7456
  [30 x 16]`. With the MSP DisplayPort OSD wired as well (below), iNav uses that one alone:
  `fc/fc_init.c:558-582` takes the first display port that opens, in the order FrSky PixelOSD, MSP
  OSD, MAX7456, so `max7456Init` is never called and the chip model stays idle (no conflict; its own
  boot poll then does not run either; the bench line's `max7456_display_writes=0`);
- an HD VTX OSD (`msp_osd`, MSP DisplayPort) on `uart1`..`uart5` (the trainer's OSD bench and the app's
  `inav-trainer-fpv-osd` preset: `uart4` = serial 3, free next to the GPS). Its iNav lines (the
  descriptor's `config_firmware.inav`, `devices/src/msp_osd.c`): `feature OSD`, `serial <n> 33554432
  115200 115200 0 115200` (FUNCTION_MSP_OSD, bit 25, alone on the port; iNav opens it at the 4th,
  peripheral baud), `set osd_video_system = HDZERO` (the 50 x 18 canvas) and an `osd_layout` that
  spreads iNav's 30-column default over 50 x 18 with the artificial horizon on. `status` then shows
  `OSD: MSP DisplayPort [50 x 18]`. The device polls MSP_FC_VARIANT every 125 ms (this board's
  default, `-M osd-poll=`): iNav draws only while a VTX talks to it. The layout is ours, not iNav's
  (iNav has no 50-column default). The device's lines come after the session profiles, so a profile
  does not move those 17 items; changing them belongs in a layer after the device's (config layers,
  not on main yet) or a firmware save from the configurator's OSD tab (bench-state rules apply). Evidence: `core/qemu/evidence/inav-osd/README.md`.

Not modelled: the USB VCP (MSP/CLI go over USART6, the target's default MSP port, so no USB is
needed); the SD card (`sdcard1`, `SD card: Startup failed`) — listed as an onboard device and left
out with a note; the RSSI and airspeed ADC inputs (read 0 V). Without a battery wired the ADC reads
0 V (`0S battery - NOT PRESENT`, no arming flag for it).
The plane FDM runs on this board (`fdm: "plane"`, the trainer).

# Spike: a conventional fixed-wing on iNav, SpeedyBee F405 WING (2026-09-30)

Research only, no code. Goal (product owner): a single-motor plane (aileron L/R, elevator, rudder, motor
+ ESC) flying **stock iNav** on an emulated **SpeedyBee F405 WING**, with a `launch` command (hand or
bungee launch to ~50 km/h) that also triggers iNav's NAV LAUNCH. Other firmwares and the Betaflight/S2 heli
presets are out of scope.

Sources (local clones; paths below are relative to them):
- **iNav** `inav/` at tag **9.1.0** (e519b69b, the latest stable release, 2026-07-09; `10.0.0-rc1`
  of 2026-09-24 is a pre-release). Paths below are relative to `inav/src/main/` unless they start
  with `target/link`, `cmake/` or `lib/`.
- **Release binary** `inav-fw/inav_9.1.0_SPEEDYBEEF405WING.hex` (GitHub release asset; there is also a
  separate `…_SPEEDYBEEF405WINGV2.hex` for the V2 board). Its data spans 0x08000000–0x08098BE4
  (610 KiB) and it leaves the config sector blank.
- **QEMU** `qemu/` v11.1.1: the tree our `core/qemu/build.sh` uses.

## Summary
- **We don't need USB.** Stock `targetConfiguration()` already puts **MSP on USART6** at 115200
  (`target/SPEEDYBEEF405WING/config.c:38`). CRSF goes on USART1 (`:36`) and GPS on USART3 (`:37`). All
  the configuration the core needs is plain CLI over USART6, then `save`. So A2 is solved with a stock
  binary, no soldering and no source changes.
- **The F405 SoC is a real new machine, not a re-parameterised nexus-xr.** The USART, SPI and I2C
  register layouts differ from the F7. iNav's F4 port uses the StdPeriph drivers, which means polled
  SPI with no gyro EXTI or DMA, interrupt-driven UARTs, an I2C v1 state machine, and PWM/OneShot motors
  (no DMA needed).
- **Our idle skip won't trigger on iNav.** Its idle scheduler never reads CYCCNT; `micros()` reads
  SysTick VAL + ICSR (`drivers/time.c:114-128`), so the detector needs a second hook.
- **The plane FDM is small.** A coefficient model with a stall blend is a few hundred lines of scalar maths
  (FlightBench's own: `docs/fdm.md` §3).
- **NAV LAUNCH works without GPS.** The "bungee" detector only needs body-X specific force
  > 1863 cm/s² (1.9 g) for > 40 ms while within 45° of level (`navigation/navigation_fw_launch.c:414-439`).
  50 km/h reached in 0.5 s is 2.8 g, which clears it.

---

## A. iNav on SPEEDYBEEF405WING

### A1. The target (`target/SPEEDYBEEF405WING/target.h`, `target.c`, `config.c`, `CMakeLists.txt`)
| item | target | source | we model? |
|---|---|---|---|
| MCU | STM32F405xG, `target_stm32f405xg` | `CMakeLists.txt:1` | new machine (B6) |
| clock | HSE 8 MHz default → PLL M8 N336 P2 = 168 MHz; APB1 timers 84 MHz, APB2 (TIM1/8/9/10/11) 168 MHz | `cmake/stm32.cmake:79`, `target/system_stm32f4xx.c:382,403-408`, `drivers/timer_stm32f4xx.c:98-111` | per-timer clock is new (ours has one `TIMER_CLK_HZ`) |
| gyro/acc | ICM42605 driver family (42605/42686P/42688P share registers), **SPI1** PA5/6/7, CS **PA4**, align CW270; no `USE_EXTI` → the gyro is **polled** by the 4 kHz GYRO task, no EXTI, no DMA | `target.h:100-104`; `drivers/accgyro/accgyro_icm42605.c:300-310,327`; `fc/config.h:32`; `drivers/bus_spi.c:204-243` | `icm42688p` ✓ (needs a test that replays iNav's init sequence) |
| baro | I2C1 (PB8/PB9), drivers BMP280/MS5611/BMP085/DPS310/SPL06. Board: **SPL06-001** (retailer spec) | `target.h:91-113` | `dps310` model, **but** autodetect tries SPL06 (`sensors/barometer.c:147`) before DPS310 (`:173`); both read ID 0x10 at 0x0D, so iNav drives our model with `barometer_spl06.c` in command mode (MEAS 0x01/0x02, `:88,108`; COEF_RDY `:178`). Register map matches (`barometer_spl06.h:24-43`); needs a replay test |
| compass | I2C1, `USE_MAG_ALL` | `target.h:115-118` | no (optional, A5) |
| OSD | MAX7456, **SPI2** PB13/PC2/PC3, CS **PB12** | `target.h:124-127` | no; MSP DisplayPort alternative (A4) |
| blackbox | SD card, **SPI3** PB3/4/5, CS PC14; logging to SD on by default | `target.h:129-135` | no (empty SPI3 bus → no card) |
| UARTs | VCP + USART1 (PA9/10), USART2 (PA2/3), USART3 (PC10/11), UART4 (PA0/1), UART5 (PC12/PD2), USART6 (PC6/7), softserial1 on PA2; `SERIAL_PORT_COUNT 8` | `target.h:55-86` | new F4 USART model (B6) |
| default ports | VCP = MSP (`io/serial.c:123`); **USART1 = RX_SERIAL CRSF** (`target.h:137-139`, `config.c:36`); USART3 = GPS (`config.c:37`); **USART6 = MSP** (`config.c:38`); all MSP ports at 115200 (`io/serial.c:117`) | | `crsf` ✓, `msp_osd` ✓ |
| outputs | 11 × `TIM_USE_OUTPUT_AUTO`: S1 TIM4_CH2 PB7, S2 TIM4_CH1 PB6, S3 TIM3_CH3 PB0, S4 TIM3_CH4 PB1, S5 TIM8_CH3 PC8, S6 TIM8_CH4 PC9, S7 TIM8_CH2N PB14, S8 TIM2_CH1 PA15, S9 TIM2_CH3 PB10, S10 TIM2_CH4 PB11, S11 TIM12_CH2 PB15; LED TIM1_CH1 PA8 | `target.c:24-41` | timers TIM2/3 exist; TIM4/8/12 are new |
| ADC | ADC1 via DMA2_Stream0: VBAT PC0, CURRENT PC1, RSSI PC5, AIRSPEED PC4 | `target.h:141-152` | stub (as on F7: DMA-driven, reads 0) |
| LEDs / beeper | LED0 PA14, LED1 PA13 (these are the SWD pins); beeper PC15 inverted; PINIO1 PC13 | `target.h:24-29,173-175` | GPIO ✓ |
| features | OSD, CURRENT_METER, VBAT, TELEMETRY, BLACKBOX, AIRMODE, TX_PROF_SEL | `target.h:159` | — |
| USB | `USE_VCP` (OTG_FS, IRQ 67) | `target.h:55`; `lib/.../stm32f4xx.h:272` | no; not needed (A2) |

New device models: **I2C v1 controller** (not a chip), **none for chips in phase 1**. MAX7456 isn't
needed (A4). GPS (UBX) is needed only for phase 4.

### A2. Configuration without USB
There's no custom-defaults blob in iNav code (`custom_defaults` shows up only in AT32 linker scripts,
`target/link/at32_*.ld`). Defaults are built in C: `pgResetFn_*` → `createDefaultConfig()` →
`targetConfiguration()` (`fc/config.c:281-298`). This runs only when the EEPROM is invalid
(`fc/config.c:381-386`, called from `fc/fc_init.c:239`). iNav then writes the EEPROM once and does
**not** reboot (unlike RotorFlight).

**EEPROM format** (`config/config_eeprom.c`, `EEPROM_CONF_VERSION 126`, `config/config_eeprom.h:24`):
- The region runs from `__config_start` to `__config_end` = `FLASH_CONFIG` **0x080E0000, 128 KiB = sector 11**
  (`target/link/stm32_flash_f405xg.ld:28-29`, `target/link/stm32_flash.ld:18-19`).
- Layout: header `u8 format` (`:57-59`), then records `{u16 size, u16 pgn, u8 version, u8 flags(0 = system, 1..3 = profile), pg[]}`
  (`:62-72`), then footer `u16 0` and CRC16-CCITT over everything before it (`:75-78,139-182`).
- Each PG is a raw `memcpy` of its C struct (`:253-312`). `pgLoad` restores a record only when its
  version matches, and keeps the defaults otherwise (`config/parameter_group.c:86-94`).
- Writing (`config/config_streamer_stm32f4.c:78-104`): erase the sector at every 16 KiB boundary
  (`FLASH_EraseSector`, `VoltageRange_3`), then `FLASH_ProgramWord`.

| option | stock binary | no source change | no soldering | effort | verdict |
|---|---|---|---|---|---|
| **(0) USART6 is MSP by default**: the core sends a CLI script over USART6 on first boot, then `save` | ✓ | ✓ | ✓ (USART6 is a pad) | small: the ctl's MSP/CLI port becomes a board parameter (it's hard-coded USART1 today, `nexus_xr_ctl.c.inc:13,179,1033`) | **use this** |
| (c′) snapshot: after (0), the core keeps sector 11 (0x080E0000) keyed by *firmware hash + script hash* and writes it next time, like the blob at `fw=` | ✓ | ✓ | ✓ | small (uses the flash model we already have) | an optimisation of (0): skips the ~2 s script on every start |
| (c) generate the EEPROM externally | ✓ | ✓ | ✓ | large and brittle: it needs every PG struct's layout, padding and version for each iNav release (`parameter_group.h`, ~100 PGs); a version mismatch silently falls back to defaults (`parameter_group.c:90`) | reject; (c′) gets the same result, written by the firmware itself |
| (a) build option `MSP_UART=SERIAL_PORT_USARTx` | ✗ (custom build) | ✓ | ✓ | trivial (`cmake/stm32.cmake:341-342` → `fc/config.c:300-305`) | not needed on this target; keep it for targets without an MSP UART |
| (b) fork `config.c` | ✗ | ✗ | ✓ | trivial | not needed |
| (d) emulate USB CDC | ✓ | ✓ | ✓ | **weeks**: QEMU has no USB device-side controller. `hcd-dwc2.c` is host-only (`hw/usb/hcd-dwc2.h:2,116-153`; host mode forced, `hcd-dwc2.c:719-727,781`); OTG_FS/HS are `unimplemented` in `stm32f405_soc.c:295-296`; there's no gadget framework. We'd need a new DWC2 device-mode model plus the StdPeriph USB stack's endpoint/FIFO behaviour plus a host bridge | reject |

Ranking by the product principle: **(0) > (c′) > (a) > (b)**. (c) and (d) are rejected.
Note: `bench.board_config` has nothing to point at for iNav. The equivalent is a **CLI script**
(`boards/speedybeef405wing/defaults.cli` + the aircraft's `models/<plane>/inav.cli`), which the
resolver assembles in place of the blob.

### A3. First flight: what arming needs
Arming flags: `fc/runtime_config.h:20-61`; checks: `updateArmingStatus()` `fc/fc_core.c:201-352`.
| requirement | source | how the core satisfies it (CLI over USART6 unless noted) |
|---|---|---|
| `platform_type = AIRPLANE` (default MULTIROTOR) | `fc/settings.yaml:1239-1244` | `set platform_type = AIRPLANE` |
| motor mix: the firmware has no plane preset (the configurator supplies it) | `flight/mixer.c:708` | `mmix 0 1.0 0 0 0` |
| servo mix: empty by default (`rate == 0` rules are skipped) | `flight/servos.c:185-237,309-330` | `smix` aileron L/R ← STABILIZED_ROLL (±), elevator ← PITCH, rudder ← YAW |
| outputs: a timer can't mix motor and servo; the motor claims S1 **and** its timer twin S2 (TIM4); servos then take S3 (TIM3_CH3), S4 (TIM3_CH4), S5 (TIM8_CH3), S6 (TIM8_CH4) | `drivers/pwm_mapping.c:255-311,313-372` | wiring: motor→S1, servos→S3..S6. The servo-index→pad order must be checked on the stand, not assumed |
| motor protocol: default **ONESHOT125** (125–250 µs pulses from CCR, no DMA); servos 50 Hz | `fc/settings.yaml:849-852,1338-1341` | read the pulse width from CCR (as our servo path does). DShot on TIM4 would need **DMA1**, which we don't model: keep ONESHOT125 or `STANDARD` |
| ARM box: no aux ranges by default (`modeActivationConditions` has no reset fn) | `fc/rc_modes.c:55`, `fc/fc_msp_box.c:53` | `aux 0 0 4 1700 2100` (ARM on ch5), plus ANGLE and NAV LAUNCH on other channels |
| throttle low to arm (unless `fixed_wing_auto_arm`) | `fc/fc_core.c:229-237`, `settings.yaml:1590` | pilot plan |
| RC link: CRSF on USART1 | `fc/fc_core.c:222-227`; `target.h:137-139` | `crsf` device on USART1 (a default) |
| ACC calibrated: needed as soon as ANGLE, NAV LAUNCH or a non-DROP failsafe is configured (`isAccRequired`, `fc/fc_core.c:130-142,292`). "Calibrated" = `accZero`/`accGain` differ from 0/4096 (`sensors/acceleration.c:651-660`). iNav calibration takes **6 positions** (`MSP_ACC_CALIBRATION` starts it, `fc/fc_msp.c:2829-2834`) | | **decide:** (i) `set acczero_x = 1` (1 LSB; stock CLI setting `settings.yaml:482`, but not a real calibration), or (ii) a real 6-point calibration driven by the motion source (6 poses; faithful; takes about half a day) |
| level (`small_angle`) | `fc/fc_core.c:248-254` | FDM on the pad |
| `nav_extra_arming_safety` (ON/ALLOW_BYPASS only, default ALLOW_BYPASS) blocks only when a GPS nav box is configured and there's no position estimate; NAV LAUNCH isn't one of those boxes | `settings.yaml:2574-2578`; `navigation/navigation.c:4764-4783,4687` | don't configure RTH/WP/POSHOLD/ALTHOLD/CRUISE in phase 3 |
| hardware failure: "requested but not detected". `AUTO` sensors are rewritten to what was found, so they never trip it; GPS trips it only with `feature GPS` and > 4 timeouts | `sensors/initialisation.c:82-104`; `sensors/diagnostics.c:172-193,226-266` | leave mag AUTO, pitot NONE; `feature -GPS` until phase 4 |
| CLI open blocks arming | `fc/cli.c:5138` | the core leaves the CLI with `save` (reboots) |

**NAV LAUNCH** (`navigation/navigation_fw_launch.c`). States (`:84-96`): WAIT_THROTTLE → (idle) → WAIT_DETECTION → DETECTED →
MOTOR_DELAY → SPINUP → IN_PROGRESS → FINISH. Detection (`:414-439`):
- `bungee` = `imuMeasuredAccelBF.x > nav_fw_launch_accel` **and** tilt ≤ `nav_fw_launch_max_angle`
- `swing` = acc.y / gyro.z > `nav_fw_launch_velocity` while |gyro.z| > 100 °/s
- `forward` = the GPS path

Any of these must hold continuously for `nav_fw_launch_detect_time`, and the throttle stick must not be
low (`:416`). `imuMeasuredAccelBF` is the raw accelerometer in cm/s², so gravity is included, scaled
from `acc.accADCf` (`flight/imu.c:947`, `sensors/acceleration.c:528-531`; ACC LPF 15 Hz,
`settings.yaml:468`). The mode is enabled by the NAVLAUNCH box or `feature FW_LAUNCH` on an AIRPLANE
(`navigation/navigation.c:5298-5300`), and needs no GPS (`:4687`).
| setting | default | unit |
|---|---|---|
| `nav_fw_launch_accel` | 1863 | cm/s² (1.9 g) |
| `nav_fw_launch_detect_time` | 40 | ms |
| `nav_fw_launch_max_angle` | 45 | deg |
| `nav_fw_launch_velocity` | 300 | cm/s (swing/GPS) |
| `nav_fw_launch_idle_thr` / `_thr` | 1000 / 1700 | µs |
| `nav_fw_launch_motor_delay` / `_spinup_time` | 500 / 100 | ms |
| `nav_fw_launch_climb_angle` | 18 | deg |
| `nav_fw_launch_end_time` / `_timeout` | 3000 / 5000 | ms |
| `nav_fw_launch_land_abort_deadband` | 100 | RC µs (stick abort) |
(`fc/settings.yaml:3012-3030` and the neighbouring `nav_fw_launch_*` entries.)

Consequence: after release the motor is **off for 0.5 s + 0.1 s spin-up**, so the FDM must glide the
plane at ~14 m/s for 0.6 s (the trainer model stalls far below that, see C8).

### A4. OSD
- `USE_MSP_DISPLAYPORT` and `USE_MSP_OSD` are built for every target (`target/common.h:109,117`),
  alongside MAX7456.
- A serial port with `FUNCTION_MSP_OSD` (bit 25 = `serial <n> 33554432 …`, `io/serial.h:59,64`) makes
  MSP DisplayPort the OSD. Init order is FrSky OSD → MSP OSD → MAX7456, first found wins
  (`fc/fc_init.c:558-580`).
- HDZERO canvas is 50×18 (`io/displayport_msp_osd.c:89-90`), the same as our `msp_osd`.
- A silent MAX7456 (SPI2 empty bus → 0xFF) doesn't hang and doesn't block arming (`drivers/max7456.c:390-420`,
  `io/displayport_max7456.c:191-198`).
- iNav pushes MSP **v1** `MSP_DISPLAYPORT` frames (`io/displayport_msp_osd.c:135,159`), which is what
  our parser expects. **Not verified:** the subcommand and attribute details against our parser (it
  was written for RotorFlight's stream). That needs a replay test.
- Firmware to target: **9.1.0**, `inav_9.1.0_SPEEDYBEEF405WING.hex`. Build the ELF from tag 9.1.0 as
  well (same sources), because `speed.py`/gdb need symbols (the hex has none).
- MSP status polling: MSP_STATUS (101) is deprecated in 9.1 and removed in 10.0
  (`msp/msp_protocol.h:234,291`), and its layout differs from RotorFlight's (`fc/fc_msp.c:472-491`).
  The core should poll `MSP2_INAV_STATUS` (0x2000, `msp/msp_protocol_v2_inav.h:21`) with MSP v2 framing.

### A5. Sensors
| sensor | boot | arm | missing → |
|---|---|---|---|
| gyro | **required**: `sensorsAutodetect()` fails (`sensors/initialisation.c:47-51`) | required (`diagnostics.c:29-37`) | no boot / permanent block |
| acc | same chip | calibration needed (A3) | block (`ACCELEROMETER_NOT_CALIBRATED`) |
| baro | optional (AUTO) | no | AUTO → NONE, silent (`initialisation.c:82-104`); every probe NACKs on I2C1, as on our F7 |
| mag | optional (AUTO) | no | same |
| pitot | NONE by default | no | — |
| GPS | USART3 by default, `feature GPS` | only with `feature GPS` and > 4 timeouts | set `feature -GPS` until the GPS model exists |

---

## B. STM32F405 machine vs our F722 machine

### B6. Differences, and what nexus_xr.c hard-codes
- **Core.** Upstream `cortex-m4` has a single-precision FPU (`target/arm/tcg/cpu-v7m.c:105-133`, mvfr0
  0x10110021) and PMSAv7 with 8 regions (`:115`). We rely on no M7 cache or TCM. **DWT isn't in upstream
  QEMU at all** (no CYCCNT in `armv7m_nvic.c`/`target/arm`), so we keep our own (`nexus_xr.c:181`,
  0xE0001000). iNav enables it (`drivers/system.c:62-75`) but only uses it for `delayNanos`
  (`drivers/time.c:68-81`).
- **Upstream `stm32f405_soc.c`** gives:
  - 1 MiB flash as **read-only ROM** (`:124`) → we need our writable `rom_device` + FLASH controller.
  - SRAM 128 K (`:137-143`) and CCM 64 K at 0x10000000 (`:145-151`).
  - Working models: SYSCFG, EXTI, USART (F2 layout), TIM2-5 with **update IRQ only, no CCR behaviour**
    (`hw/timer/stm32f2xx_timer.c:47-67`), SPI **without IRQ/DMA** (`hw/ssi/stm32f2xx_spi.c:79-80,129-131`),
    a fake ADC, and an RCC with only reset/enable bits (`hw/misc/stm32_rcc.c:2`).
  - Everything else is `unimplemented` (`:257-298`): GPIO, DMA1/2, I2C1-3, flash controller, TIM1/8/12, USB.
  - `num-irq` is 96 (`:154`); the F405 has 82, since FPU_IRQn = 81 (`lib/main/STM32F4/.../stm32f4xx.h:286`).
- **Recommendation:** don't use `stm32f405_soc` as the base (read-only flash, no GPIO/DMA/I2C, timers
  without CCR). Split `nexus_xr.c` into a **SoC description table + shared models** and add an `f405`
  description, reusing only upstream `stm32f2xx_usart` (F4 register layout; IRQ = SR & CR1,
  `hw/char/stm32f2xx_usart.c:46-54`).
- **Flash sectors (F405):** 4×16 K, 64 K, 7×128 K; the config lives in **sector 11**
  (`config/config_streamer_stm32f4.c:26-39`). The FLASH controller registers (KEYR/SR/CR at
  0x40023C00, SNB bits 3..6) are laid out as on the F7, so only the sector table changes. A
  side note in the same file (`:55-58`): the sector-9/10 bounds are checked in the wrong order. That
  doesn't matter for sector 11.
- **Clock:** `SystemInit`/`SetSysClock` spin on HSERDY, PLLRDY and SWS (`target/system_stm32f4xx.c:645-741`).
  Our RCC "RDY follows ON, SWS follows SW" behaviour covers these.
- **UID** 0x1FFF7A10 (`platform.h:77`), vs 0x1FF07A10 on the F7 (`nexus_xr.c:98`).

| nexus_xr.c (F722) | line | F405 (iNav SPEEDYBEEF405WING) | reusable? |
|---|---|---|---|
| SYSCLK 216 / PCLK1 54 / PCLK2 108 MHz; one timer clock = SYSCLK | 85-87, 147 | 168 / 42 / 84 MHz; timers APB1 84, APB2 168 | parameter + **per-timer** clock |
| ITCM 16 K @0, flash alias @0x00200000 | 89-91 | none; flash aliased at 0 (boot) | description |
| flash 512 K; sectors 4×16 K, 64 K, 3×128 K | 93, 194-197 | 1 M; 4×16 K, 64 K, 7×128 K; config @0x080E0000 | table |
| SRAM 256 K @0x20000000 | 94-95 | 128 K + **CCM 64 K @0x10000000** + BKPSRAM 4 K (`stm32_flash_f405xg.ld:31-33`) | description |
| SYSMEM, UID 0x1FF07A10, flash size 0x1FF07A22 | 96-99 | 0x1FFF0000, UID 0x1FFF7A10, flash size 0x1FFF7A22 | description |
| NUM_IRQ 104 | 101 | 82 | description |
| stub windows APB/AHB1, AHB2, FMC | 104-109 | same bases (AHB2 = OTG_FS at 0x50000000) | ✓ |
| RCC/FLASHC/PWR/ADC/SYSCFG/RTC/BKPSRAM bases | 112-118 | same addresses on F4 | ✓ (register subsets differ slightly, e.g. PWR CSR) |
| SPI1 @0x40013000: F7 FIFO, FRXTH, data packing | 126 | F4 SPI: no FIFO, RXNE per frame, 8/16-bit DFF; polled (`drivers/bus_spi.c:204-243`) | **F4 variant** (small) |
| SPI2..5 empty bus | 121-124 | SPI2 (MAX7456), SPI3 (SD) empty bus | ✓ |
| IMU CS PA4, INT PB8 | 142-145 | CS PA4; **no INT used** | ✓ (board data) |
| EXTI + IRQs 6..10/23/40 | 127, 178 | same | ✓ |
| DMA2 @0x40026400, IRQs 56-60/68-70 | 130, 179 | same address and IRQs; F4 CHSEL is 3 bits (F7: 4). iNav uses DMA2 only for ADC here | ✓ (ADC stream stays passive) |
| I2C3 v2 @0x40005C00, IRQ 72/73 (`stm32_i2c.c.inc`) | 131-133 | **I2C1 v1** @0x40005400, IRQ **31/32** (`stm32f4xx.h:236-237`); SR1/SR2/DR, event-driven ISR (`drivers/bus_i2c_stm32f40x.c`) | **new model** (the bus/slave table and the DPS310 hook are reusable) |
| TIM1/2/3 modelled | 148-152 | motor S1/S2 **TIM4** (IRQ 30); servos TIM3 (29), **TIM8** (CC 46), TIM2 (28), **TIM12** (43) | add TIM4/8/12 + per-timer clock |
| servo_out = TIM3 CH1/2/3, TIM2 CH1; motor TIM1_CH2 DShot DMA | 159-164 | from `target.c`: S1..S11 table; motor = ONESHOT125 CCR pulse, **no DMA** | table from board data |
| CRSF default UART5 | 167 | USART1 | board data |
| USART addresses; IRQs 37,38,39,52,53,71,82,83; model `stm32l4x5-usart` (ISR/RDR/TDR) | 186-191, 4000 | same addresses and IRQs (`stm32f4xx.h:242-276`), but the **F4 register layout** (SR 0x00, DR 0x04, BRR, CR1 0x0C…) → upstream `stm32f2xx_usart` | **swap the model**. The CRSF early release hooks the RQR.RXFRQ write, which the F4 lacks: hook the DR read instead (the RXNE ISR reads DR, `drivers/serial_uart_stm32f4xx.c:218-222`), otherwise it falls back to wire time (costs a deadline per byte) |
| DWT 0xE0001000, DBGMCU 0xE0042000 | 181-182 | same | ✓ |
| cortex-m7, init-nsvtor | 3887-3893, 4555-4561 | cortex-m4 | description |
| custom-defaults blob at 0x08002808 | README "Firmware" | **none** → CLI script / sector-11 snapshot (A2) | resolver change |
| ctl MSP master + CLI on USART1, RotorFlight MSP_STATUS | `nexus_xr_ctl.c.inc:13,51,1159` | USART6, `MSP2_INAV_STATUS` | parameter + iNav decoder |
| `env` (wind, pressure) not modelled (log line) | `nexus_xr_ctl.c.inc:2413-2414` | a plane needs wind in the FDM (airspeed ≠ ground speed) | FDM work (C8) |

### B7. Idle skip on iNav
- iNav's idle pass is `main()` → `scheduler()` → `micros()` (`main.c:76`, `scheduler/scheduler.c:215,238,246`).
- `micros()` reads **SCB->ICSR**, **BASEPRI** and **SysTick->VAL** in a retry loop against
  `sysTickUptime` (`drivers/time.c:114-128`).
- **No CYCCNT read** in that path: `ticks()` → CYCCNT is only used by `delayNanos` (`drivers/time.c:68-81`).
- So our detector (a CYCCNT read with a period of PC + instruction-count signatures, README "Idle skip")
  never fires on iNav. The same algorithm needs a **second trigger on SysTick VAL reads**. The SysTick
  region is already wrapped for the ledger (`prof_wrap_mr(&s->armv7m.systickmem…)`, `nexus_xr.c:867`),
  which is the natural hook. Everything else (budget skip, virtual-deadline bound, NVIC interception)
  carries over.
- Expected load is lighter than RotorFlight: GYRO task 4 kHz polled (`fc/config.h:32`), PID looptime
  500 µs (`settings.yaml:238-241`), no gyro EXTI/DMA deadlines. But each polled SPI gyro read costs
  about 2 MMIO flag polls per byte (`bus_spi.c:204-215`). Measure it; don't assume.

---

## C. Fixed-wing FDM

### C8. The fixed-wing FDM
(Rewritten 2026-10-24.) This section held the plan to port a third-party simulator's plane model; that plan was
carried out and then dropped for licence reasons (`docs/decisions.md`, 2026-10-24). The fixed-wing FDM FlightBench
ships is its own, specified in `docs/fdm.md` §3: the linear-coefficient model with a stall blend, a fixed-pitch
prop on the ESC's rpm, wind, wheels that roll forward only, the holder and the launch. The trainer's coefficients
are estimates from its geometry (`models/trainer/README.md`), not any product's set.

### C9. Launch
- Other simulators model the launch as a timed force on the body (catapult, bungee, throw profiles); a bungee of
  0.7 g would **not** trigger iNav's 1.9 g detector, so FlightBench's launch is a kinematic ramp with a chosen
  acceleration instead.

**Proposed protocol command** (`docs/protocol.md` Client → core):
```json
{"type":"launch","speed_kmh":50,"duration_s":0.5,"heading_deg":null,"pitch_deg":10}
```
- **Semantics:** a *rail/hand* constraint in the FDM for `duration_s`.
  - Attitude is held at (current heading or `heading_deg`, `pitch_deg`, roll 0) with zero body rates.
  - Velocity along that direction ramps linearly from 0 to `speed_kmh` (constant a = v/duration).
  - Then the constraint is released into free flight. Aerodynamic forces are computed during the ramp
    but only the along-rail component acts.
  - Events `launch {phase:"start"|"release"}`.
- **What the IMU sees during the ramp:** specific force body-X = a + g·sin(pitch), body-Z = −g·cos(pitch).
  For 50 km/h in 0.5 s at 10° pitch that's 27.8 + 1.7 = 29.5 m/s² ≈ 3.0 g, well above the 18.3 m/s²
  threshold, for 500 ms (≫ 40 ms), within the 45° limit. **iNav's bungee detector fires.**
- **Validation rule in the core:** refuse (or warn about) profiles with a < 1.2 × `nav_fw_launch_accel`
  (22.4 m/s²), i.e. `duration_s` > 0.62 s for 50 km/h. The threshold comes from the firmware settings
  the core already knows.
- After release, the motor is off for 0.6 s (A3), so the FDM must not stall at 13.9 m/s. The trainer
  numbers give a comfortable margin (V at CL 1.2 ≈ 7.7 m/s for 2 kg, 0.45 m²).
- Hand-launch realism: a throw is ~0.2–0.4 s at 3–5 g. Keep `duration_s` a free parameter; the default
  is 0.5 s.

### C10. `aircraft.json` for a plane
```json
{"id":"trainer","name":"Trainer (1.9 m electric trainer)","class":"electric trainer plane","fdm":"plane",
 "actuators":["aileron_l","aileron_r","elevator","rudder","motor"],
 "fdm_inputs":["aileron_l","aileron_r","elevator","rudder"],
 "centre_us":{"aileron_l":1500,"aileron_r":1500,"elevator":1500,"rudder":1500},
 "travel_us":{"aileron_l":500,"aileron_r":500,"elevator":500,"rudder":500},
 "throttle":"motor",
 "esc":{"kv":1000,"poles":14,"tau_s":0.05,"max_current_a":40,"idle_current_a":0.3,"voltage_v":14.8},
 "prop":{"diameter_in":9,"pitch_in":6,"kt":null},
 "firmware_cli":"models/trainer/inav.cli",
 "fdm_params":{"mass_kg":2.0,"s_m2":0.45,"b_m":1.88,"c_m":0.24,"cg_offset_m":[-0.15,0,-0.05],
   "c_lift_0":0.56,"c_lift_a":6.9,"alpha_stall":0.4712,"mcoeff":50,"c_drag_p":0.1,"oswald":0.9,
   "c_y_b":-0.98,"c_y_deltar":-0.2,
   "c_l_p":-1.0,"c_l_b":-0.12,"c_l_r":0.14,"c_l_deltaa":0.25,"c_l_deltar":-0.037,
   "c_m_0":0.045,"c_m_a":-0.7,"c_m_q":-20,"c_m_deltae":1.0,
   "c_n_b":0.25,"c_n_p":0.022,"c_n_r":-1,"c_n_deltar":0.1,
   "servo_delay_ms":10,"servo_speed_s60":0.1,"frame_height_m":0.1,"ground_friction":0.3}}
```
- **Ailerons:** the FDM uses (L − R)/2 as roll input and flags a same-sign pair as a mis-mix. Aileron
  droop/flaperon isn't modelled. Direction and `reversed` come from the wiring, as with the heli.
- The ESC/prop numbers are placeholders to size in phase 2. The trainer's coefficients assume their
  own geometry, so keep them together.
- **Visual:** a Godot placeholder (fuselage box, wing slab, tail slabs, axis gizmo) moving with FDM
  pose. Control surfaces rotate by the servo µs → deflection. Enough for the first flight.

---

## D. Plan

| phase | content | depends on | effort (agent-days) | acceptance evidence |
|---|---|---|---|---|
| **1 F405 machine → CLI** | split nexus_xr.c into a SoC description + shared models; `speedybee-f405-wing` board (board.json pads from `target.h`/`target.c`; there's no unified config); 1 MiB writable flash + sector table; CCM, UID; per-timer clock, TIM4/8/12; F4 USART (upstream `stm32f2xx_usart` + DR-read hook for CRSF); F4 SPI1 variant; **I2C1 v1** controller + DPS310/SPL06 replay test with iNav's driver; ICM42688 replay test with iNav's init; ctl MSP/CLI port = USART6 + `MSP2_INAV_STATUS`; resolver: CLI script in place of the blob, sector-11 snapshot (A2 c′); SysTick-VAL idle trigger | — | 5–7 | `evidence/inav-boot-*.log`: stock 9.1.0 hex boots; `status` over USART6 shows `ICM42688P`, `BARO=SPL06`, I2C errors = the autodetect probes only; `tasks` rates (GYRO 4 kHz); arming flags only CLI/RC before setup; config survives reset (sector 11); idle skip % with host s per 10 virtual s; RS7 flight log hash unchanged (the F722 machine isn't regressed) |
| **2 plane FDM** | `fdm/src/plane.c` port (C8) + ESC/prop thrust + wind + launch constraint; host tests | — (parallel with 1) | 2–3 | `make -C fdm test`: trim glide (sink rate, speed stable ±5 %), elevator/aileron/rudder step responses with the right signs and damping, stall and recovery, launch profile → IMU specific force table (≥ 2.8 g for 0.5 s), ground roll/stop, determinism |
| **3 iNav flies it** | wiring (motor S1 ONESHOT125 from CCR, servos S3..S6), `inav.cli` (platform, mmix/smix, aux ARM/ANGLE/NAV LAUNCH, ACC calibration per the decision in A3, `feature -GPS`), `launch` protocol command + app button, placeholder visual, `presets/trainer-first-flight.json` | 1, 2 | 3–4 | `evidence/plane-launch-*.log` (like the RS7 lift-off): arm → throttle up → `launch` → iNav reports NAV LAUNCH detected (`MSP2_INAV_STATUS` mode) → motor after 0.5 s → climb at ~18° → ACRO/ANGLE stick doublets followed; FDM log determinism across two runs |
| **4 OSD / GPS / nav** | `msp_osd` replay test with iNav's stream + `FUNCTION_MSP_OSD` wiring; GPS UBX model on USART3 (+ NED→geodetic from the heli); then ALTHOLD/RTH | 3 | OSD 0.5–1; GPS 2–3 | OSD canvas frames in the app; GPS fix, `nav_extra_arming_safety` passes, RTH home distance |

Total to a first launched flight: about 10–14 agent-days, plus review (per the review-after-agents rule).

### Top 3 risks
1. **I2C v1 plus the rest of the F4 peripheral set is more than a "port".** StdPeriph's event-driven
   I2C (SB/ADDR/BTF/RxNE sequencing) and the UART/SPI differences are new models, and nexus_xr.c is
   4.7k lines written for one SoC. Mitigation: the baro is optional (AUTO → NONE), so phase 1 can
   reach `status` with I2C1 as a NACKing stub and add the controller after; do the refactor behind the
   RS7 flight-hash regression.
2. **Idle skip and speed.** Without the SysTick trigger, every idle pass runs under icount (RotorFlight
   without the skip: 27 host s per 10 virtual s, README "Idle skip"). The polled SPI gyro at 4 kHz is
   MMIO-heavy. If the SysTick signature turns out to be less regular than CYCCNT (the `micros()` retry
   loop), the fallback is `looptime`/gyro rate settings — those are behaviour changes and need an
   explicit OK.
3. **Launch plus FDM fidelity.** The trainer's coefficients are estimates
   sigmoid; the static-thrust model has no airspeed falloff. iNav's 0.6 s motor-off glide and its
   18° climb are sensitive to both. Mitigation: phase-2 behavioural tests on the launch → glide →
   climb sequence before any firmware is involved; tune inertia and thrust from a real trainer's
   numbers, not by eye.

Open decisions for the product owner: ACC calibration (A3: `acczero_x = 1` vs a real 6-point
calibration via the motion source); iNav 9.1.0 vs 10.0.0 once it's released (10.0 removes MSP_STATUS,
which A4 already avoids).

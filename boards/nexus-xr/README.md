# RadioMaster Nexus-XR (STM32F722)

- `NEXUS_XR.config` — the unified board config embedded in the firmware (RotorFlight 4.6,
  `RDMS-NEXUS_XR`). This is the wiring seed: resources → pins, timers, DMA, serial defaults. The core
  writes it into the firmware's custom-defaults area at boot (`core/qemu/README.md` → *Firmware: what is
  needed for MSP on a UART*).
- `board.json` — what the config does not say: the labels printed on the board per pad (`null` where
  not confirmed; `hint` = the likely one), extra flags (`5v` on PORT-A/B/C), the USB pad, the onboard
  devices (`pad`, `device`, `detachable`, `part`) and labels without a known resource (`unmapped`:
  AUX). Sources are listed in the file. `core/qemu/scripts/wiring.py` merges both into the pad table.

The emulated board itself (memory map from the firmware's `stm32_flash_f722.ld`: ITCM RAM 16K @0x0,
flash 512K @0x08000000 aliased at 0x00200000, SRAM 256K @0x20000000; USART1-6, SPI1-3, I2C1-3, DMA1/2,
TIM1-14, EXTI, SYSCFG, RCC, RTC, RNG, DWT at 216 MHz, ADC stub) is the `nexus-xr` machine in `core/qemu/`.

Firmware: the official RotorFlight 4.6.0 release hex for this target flies the shipped preset
(`firmware/FIRMWARE.md`); the OSD preset in the repository uses the `STM32F7X2_OSD` build of the
`Myatlev/rotorflight-firmware-osd` fork (unified target + embedded config + OSD).

Boot quirk: on the first boot the EEPROM is invalid, the firmware writes its custom defaults to the flash
sector at 0x08004000 and resets itself (`NVIC_SystemReset`); the machine keeps the flash across that reset,
so the second boot finds a valid config.

/*
 * FlightBench STM32 machines: SoC and board descriptions.
 *
 * Included by nexus_xr.c. One machine source serves every FlightBench STM32 board; what differs
 * between the chips and between the boards is data here, the peripheral models in nexus_xr.c
 * read it (s->soc, s->board):
 *
 *   FbSoc    the chip: CPU type, clock tree (per-bus timer clocks), memory map (flash + its sector
 *            table, RAM regions, system memory with UID / flash size), NVIC size, DBGMCU IDCODE,
 *            and the peripheral instances with base address, IRQ and model variant (USART register
 *            layout, SPI1 variant, I2C variant, timers with their bus). Peripherals at the same
 *            address on every STM32F4/F7 (GPIO, EXTI, SYSCFG, DMA2, RCC, FLASH, PWR, ADC, RTC,
 *            BKPSRAM, DWT, DBGMCU, the stub windows) are constants in nexus_xr.c.
 *   FbBoard  the board on that chip: the QEMU machine type, the IMU's chip select / interrupt
 *            pins, the outputs (timer channel per servo pad, the ESC's output and how it is driven),
 *            which USARTs the receiver / OSD device may sit on, the MSP/CLI host USART, how the
 *            firmware is configured (custom-defaults blob or a CLI script), the config sector.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

typedef enum {
    FB_USART_L4X5,      /* STM32F7/L4 layout: CR1 0x00, ISR 0x1C, RDR 0x24, TDR 0x28 (QEMU stm32l4x5) */
    FB_USART_F2XX,      /* STM32F2/F4 layout: SR 0x00, DR 0x04, BRR 0x08, CR1 0x0C (QEMU stm32f2xx) */
} FbUsartModel;

typedef enum {
    FB_SPI_F7,          /* 4-byte RX FIFO, FRXTH, data packing, DS in CR2 */
    FB_SPI_F4,          /* one RX frame, RXNE per frame, DFF 8/16 bit in CR1 */
} FbSpiModel;

typedef enum {
    FB_I2C_V2,          /* F7 I2C (TIMINGR, ISR/ICR, NBYTES): stm32_i2c.c.inc, with the baro */
    FB_I2C_V1,          /* F4 I2C (SR1/SR2/DR, SB/ADDR/BTF events): stm32_i2c_v1.c.inc */
} FbI2cModel;

typedef enum { FB_APB1, FB_APB2 } FbBus;

typedef struct { const char *name; hwaddr base; uint32_t size; } FbSocRam;
typedef struct { const char *name; hwaddr base; int irq; FbBus bus; bool uart; } FbSocUsart;
/* adv: an advanced-control timer (TIM1/TIM8): its outputs also need BDTR.MOE */
typedef struct { const char *name; hwaddr base; bool is32; FbBus bus; bool adv; } FbSocTim;
typedef struct { hwaddr base; const char *name; } FbSocBlock;

#define FB_SOC_MAX_RAM  3

typedef struct FbSoc {
    const char *name;
    const char *cpu_type;
    uint64_t sysclk_hz, pclk1_hz, pclk2_hz;
    uint64_t tim_clk_hz[2];         /* timer kernel clock of the APB1 / APB2 timers */
    int num_irq;
    bool bitband;                   /* Cortex-M3/M4 bit-band aliases of SRAM and peripherals */
    uint32_t dbgmcu_idcode;
    uint32_t pwr_cr_reset, optcr_reset;
    /* memory */
    hwaddr flash_base;
    uint32_t flash_size;
    const char *flash_alias_name;   /* the second view of the flash (NULL: none) */
    hwaddr flash_alias;
    const uint32_t *flash_sector;   /* sector start offsets, nsectors + 1 entries (the last = size) */
    int nsectors;
    FbSocRam ram_low;               /* RAM mapped below the flash (F7 ITCM); size 0: none */
    FbSocRam ram[FB_SOC_MAX_RAM];   /* the others, size 0 ends the list */
    hwaddr sysmem_base;
    uint32_t sysmem_size;
    hwaddr uid_addr, flashsize_addr;
    /* peripherals */
    FbUsartModel usart_model;
    const FbSocUsart *usart;
    int nusart;
    FbSpiModel spi_model;           /* SPI1 (the gyro's bus) and SPI2 when modelled */
    hwaddr spi2_base;               /* SPI2 modelled (an OSD chip on it); 0: in spi_empty */
    const hwaddr *spi_empty;        /* SPIs with nothing on the bus */
    int nspi_empty;
    const FbSocTim *tim;            /* the timers modelled beyond storage */
    int ntim;
    FbI2cModel i2c_model;
    const char *i2c_name;           /* the modelled I2C controller: region/timer name suffix */
    hwaddr i2c_base;
    int i2c_ev_irq, i2c_er_irq;
    int exti_irq[7];                /* EXTI0..4, EXTI9_5, EXTI15_10 */
    int dma2_irq[8];
    const FbSocBlock *blocks;       /* names for the stub window's first-access log */
    int nblocks;
    hwaddr usb_hs_base;             /* USB OTG HS window (0: none) for the log names */
} FbSoc;

/* A board output: soc timer index and channel (1..4), complementary (CHxN) or not, and the pad's
 * pin (GPIO port 0 = A, pin) with the alternate function that routes the channel to it */
typedef struct { int tim; int ch; bool n; int port, pin, af; } FbOutput;

#define FB_MAX_SERVOS   11

typedef struct FbBoard {
    const char *machine;            /* QEMU machine type, the region/log prefix, hello's board */
    const char *desc;
    const FbSoc *soc;
    int imu_cs_port, imu_cs_pin;    /* the IMU's chip select (GPIO port 0 = A) */
    int imu_int_port, imu_int_pin;  /* INT1 (-1: not wired, the firmware polls the IMU) */
    int osd_cs_port, osd_cs_pin;    /* a MAX7456 on SPI2 (-1: none) */
    const FbOutput *servo;          /* servo pads SERVO 1..n (-M servos= mask bit i = servo[i]) */
    int nservo;
    FbOutput motor;                 /* the ESC's output */
    bool motor_dshot;               /* DShot through a DMA2 stream (else a pulse width from CCR) */
    int crsf_default;               /* USART index of the receiver without -M crsf= (-1: none) */
    uint32_t crsf_uarts;            /* USART index bits -M crsf= accepts */
    uint32_t osd_uarts;             /* ... and -M osd= */
    bool osd_poll;                  /* the OSD device's MSP_FC_VARIANT poll by default (-M osd-poll=):
                                     * the board's firmware family (board.json) needs it, iNav */
    const char *uart_range;         /* their names in the option errors, "uart3..uart6" */
    int msp_uart;                   /* the MSP/CLI host USART (serial_hd(0), the control server's) */
    bool cli_config;                /* configured by a CLI script over the MSP UART, not a blob */
    uint32_t cfg_off, cfg_len;      /* the firmware's config area in flash (reset all erases it) */
    bool baro;                      /* the baro model on the I2C controller at 0x76 */
    bool baro_spl06;                /* ... the SPL06-001 (devices/src/spl06.c), else the DPS310 */
    bool mag;                       /* a QMC5883L compass may be wired to the I2C controller */
    uint32_t gps_uarts;             /* USART index bits a u-blox GPS may be wired to (-M gps=) */
    bool adc;                       /* ADC1 + DMA2 stream 0 modelled (the battery meter), else the
                                     * stub's "always done" registers */
    double vbat_divider;            /* VBAT pad -> ADC pin (the target's vbat_scale / 100) */
    double curr_mv_per_a;           /* the current sensor's output (current_meter_scale / 10) */
    int adc_vbat_ch, adc_curr_ch;   /* their ADC1 channels */
    const char *const *actuators;   /* default actuator names of servo1..n + motor (NULL: servoN) */
    bool idle_systick;              /* the idle skip's trigger: SysTick VAL reads (else DWT CYCCNT) */
    int imu_align;                  /* the firmware's IMU alignment (FB_ALIGN_*): the FDM's body
                                     * frame reaches the chip turned back by it */
} FbBoard;

/* iNav/Betaflight sensor_align_e (drivers/sensor.h) */
enum { FB_ALIGN_CW0 = 1, FB_ALIGN_CW90 = 2, FB_ALIGN_CW180 = 3, FB_ALIGN_CW270 = 4 };

/* ---- STM32F722 (RM0431) ------------------------------------------------------------------- */

/* sectors 0-3 16K, 4 64K, 5-7 128K */
static const uint32_t f722_flash_sector[] = {
    0x00000, 0x04000, 0x08000, 0x0C000, 0x10000, 0x20000, 0x40000, 0x60000,
    0x80000,
};

static const FbSocUsart f722_usart[] = {
    { "USART1", 0x40011000, 37, FB_APB2, false }, { "USART2", 0x40004400, 38, FB_APB1, false },
    { "USART3", 0x40004800, 39, FB_APB1, false }, { "USART4", 0x40004C00, 52, FB_APB1, false },
    { "USART5", 0x40005000, 53, FB_APB1, false }, { "USART6", 0x40011400, 71, FB_APB2, false },
    { "UART7", 0x40007800, 82, FB_APB1, true }, { "UART8", 0x40007C00, 83, FB_APB1, true },
};

static const hwaddr f722_spi_empty[] = { 0x40003800, 0x40003C00, 0x40013400, 0x40015000 };

/* RotorFlight's timerClock() is SystemCoreClock for every F7 timer (TIMPRE set: the APB1 timers
 * also run at 216 MHz) */
static const FbSocTim f722_tim[] = {
    { "TIM1", 0x40010000, false, FB_APB2, true }, { "TIM2", 0x40000000, true, FB_APB1 },
    { "TIM3", 0x40000400, false, FB_APB1 },
};

static const FbSocBlock f722_blocks[] = {
    { 0x40000000, "TIM2" }, { 0x40000400, "TIM3" }, { 0x40000800, "TIM4" },
    { 0x40000C00, "TIM5" }, { 0x40001000, "TIM6" }, { 0x40001400, "TIM7" },
    { 0x40001800, "TIM12" }, { 0x40001C00, "TIM13" }, { 0x40002000, "TIM14" },
    { 0x40002400, "LPTIM1" }, { 0x40002800, "RTC/BKP" },
    { 0x40002C00, "WWDG" }, { 0x40003000, "IWDG" }, { 0x40003800, "SPI2" },
    { 0x40003C00, "SPI3" }, { 0x40004000, "SPDIFRX" },
    { 0x40005400, "I2C1" }, { 0x40005800, "I2C2" }, { 0x40005C00, "I2C3" },
    { 0x40006400, "CAN1" }, { 0x40007000, "PWR" }, { 0x40007400, "DAC" },
    { 0x40010000, "TIM1" }, { 0x40010400, "TIM8" },
    { 0x40011C00, "SDMMC2" }, { 0x40012000, "ADC" }, { 0x40012C00, "SDMMC1" },
    { 0x40013000, "SPI1" }, { 0x40013400, "SPI4" }, { 0x40013800, "SYSCFG" },
    { 0x40013C00, "EXTI" }, { 0x40014000, "TIM9" }, { 0x40014400, "TIM10" },
    { 0x40014800, "TIM11" }, { 0x40015000, "SPI5" }, { 0x40015800, "SAI1" },
    { 0x40015C00, "SAI2" },
    { 0x40020000, "GPIOA" }, { 0x40020400, "GPIOB" }, { 0x40020800, "GPIOC" },
    { 0x40020C00, "GPIOD" }, { 0x40021000, "GPIOE" }, { 0x40021400, "GPIOF" },
    { 0x40021800, "GPIOG" }, { 0x40021C00, "GPIOH" }, { 0x40022000, "GPIOI" },
    { 0x40023000, "CRC" }, { 0x40023800, "RCC" }, { 0x40023C00, "FLASH" },
    { 0x40024000, "BKPSRAM" }, { 0x40026000, "DMA1" }, { 0x40026400, "DMA2" },
    { 0x40040000, "USB_OTG_HS" }, { 0x50000000, "USB_OTG_FS" },
    { 0x50060800, "RNG" }, { 0xA0000000, "FMC" }, { 0xA0001000, "QUADSPI" },
};

static const FbSoc soc_stm32f722 = {
    .name = "STM32F722",
    .cpu_type = ARM_CPU_TYPE_NAME("cortex-m7"),
    .sysclk_hz = 216000000, .pclk1_hz = 54000000, .pclk2_hz = 108000000,
    .tim_clk_hz = { 216000000, 216000000 },
    .num_irq = 104,
    .dbgmcu_idcode = 0x10016452,    /* DEV_ID 0x452 (F72x/F73x), REV Z */
    .pwr_cr_reset = 0x0000C000,     /* CR1: VOS=11 */
    .optcr_reset = 0xC0AAFFFD,
    .flash_base = 0x08000000, .flash_size = 512 * KiB,
    .flash_alias_name = "flash.itcm", .flash_alias = 0x00200000,     /* the ITCM flash interface */
    .flash_sector = f722_flash_sector, .nsectors = ARRAY_SIZE(f722_flash_sector) - 1,
    .ram_low = { "itcm", 0x00000000, 16 * KiB },
    .ram = { { "sram", 0x20000000, 256 * KiB } },                  /* DTCM + SRAM1 + SRAM2 */
    .sysmem_base = 0x1FF00000, .sysmem_size = 1 * MiB,
    .uid_addr = 0x1FF07A10, .flashsize_addr = 0x1FF07A22,
    .usart_model = FB_USART_L4X5, .usart = f722_usart, .nusart = ARRAY_SIZE(f722_usart),
    .spi_model = FB_SPI_F7,
    .spi_empty = f722_spi_empty, .nspi_empty = ARRAY_SIZE(f722_spi_empty),
    .tim = f722_tim, .ntim = ARRAY_SIZE(f722_tim),
    .i2c_model = FB_I2C_V2, .i2c_name = "i2c3", .i2c_base = 0x40005C00,
    .i2c_ev_irq = 72, .i2c_er_irq = 73,
    .exti_irq = { 6, 7, 8, 9, 10, 23, 40 },
    .dma2_irq = { 56, 57, 58, 59, 60, 68, 69, 70 },
    .blocks = f722_blocks, .nblocks = ARRAY_SIZE(f722_blocks),
    .usb_hs_base = 0x40040000,
};

/* ---- RadioMaster Nexus-XR (STM32F722, RotorFlight) ---------------------------------------- */

/* NEXUS_XR.config resource + timer lines (the session's wiring decides what is connected;
 * core/qemu/scripts/wiring.py MODELLED): SERVO 1..4 = B04 TIM3_CH1, B05 TIM3_CH2, B00 TIM3_CH3,
 * A15 TIM2_CH1; MOTOR 1 = A09 TIM1_CH2 (DShot, DMA2 stream 6). AF (DS11853 table 9): TIM1/2 AF1,
 * TIM3 AF2 */
static const FbOutput nexus_xr_servo[] = {
    { 2, 1, false, 1, 4, 2 }, { 2, 2, false, 1, 5, 2 }, { 2, 3, false, 1, 0, 2 },
    { 1, 1, false, 0, 15, 1 },
};

static const char *const nexus_xr_actuators[] = {
    "swash_a", "swash_b", "swash_c", "tail", "motor"
};

static const FbBoard board_nexus_xr = {
    .machine = "nexus-xr",
    .desc = "RadioMaster Nexus-XR (STM32F722, Cortex-M7) for FlightBench",
    .soc = &soc_stm32f722,
    .imu_cs_port = 0, .imu_cs_pin = 4,          /* GYRO_CS A04 */
    .imu_int_port = 1, .imu_int_pin = 8,        /* GYRO_EXTI B08 */
    .osd_cs_port = -1, .osd_cs_pin = -1,
    .servo = nexus_xr_servo, .nservo = ARRAY_SIZE(nexus_xr_servo),
    .motor = { 0, 2, false, 0, 9, 1 }, .motor_dshot = true,
    .crsf_default = 4,                          /* UART5: the built-in ELRS receiver */
    /* USART1 is the MSP host socket, USART2 (TELEM) is RX-only on the Nexus-XR */
    .crsf_uarts = 0x3C, .osd_uarts = 0x3C, .uart_range = "uart3..uart6",
    .msp_uart = 0,
    .cli_config = false,
    .cfg_off = 0x4000, .cfg_len = 0x4000,       /* sector 1 */
    .baro = true,
    .actuators = nexus_xr_actuators,
    .imu_align = FB_ALIGN_CW180,                /* NEXUS_XR.config gyro_1_sensor_align CW180 */
};

/* ---- STM32F405xG (RM0090, DS8626) --------------------------------------------------------- */

/* sectors 0-3 16K, 4 64K, 5-11 128K (iNav config_streamer_stm32f4.c: the config is sector 11) */
static const uint32_t f405_flash_sector[] = {
    0x00000, 0x04000, 0x08000, 0x0C000, 0x10000, 0x20000, 0x40000, 0x60000,
    0x80000, 0xA0000, 0xC0000, 0xE0000, 0x100000,
};

/* stm32f4xx.h USARTx_IRQn: USART1 37, USART2 38, USART3 39, UART4 52, UART5 53, USART6 71 */
static const FbSocUsart f405_usart[] = {
    { "USART1", 0x40011000, 37, FB_APB2, false }, { "USART2", 0x40004400, 38, FB_APB1, false },
    { "USART3", 0x40004800, 39, FB_APB1, false }, { "UART4", 0x40004C00, 52, FB_APB1, true },
    { "UART5", 0x40005000, 53, FB_APB1, true }, { "USART6", 0x40011400, 71, FB_APB2, false },
};

static const hwaddr f405_spi_empty[] = { 0x40003C00 };  /* SPI3 (SPI2 is modelled: the OSD chip) */

/* APB1 = 42 MHz with prescaler 4: its timers run at 84 MHz; APB2 = 84 MHz: 168 MHz (iNav
 * drivers/timer_stm32f4xx.c timerClock) */
static const FbSocTim f405_tim[] = {
    { "TIM1", 0x40010000, false, FB_APB2, true }, { "TIM2", 0x40000000, true, FB_APB1 },
    { "TIM3", 0x40000400, false, FB_APB1 }, { "TIM4", 0x40000800, false, FB_APB1 },
    { "TIM8", 0x40010400, false, FB_APB2, true }, { "TIM12", 0x40001800, false, FB_APB1 },
};

static const FbSocBlock f405_blocks[] = {
    { 0x40000000, "TIM2" }, { 0x40000400, "TIM3" }, { 0x40000800, "TIM4" },
    { 0x40000C00, "TIM5" }, { 0x40001000, "TIM6" }, { 0x40001400, "TIM7" },
    { 0x40001800, "TIM12" }, { 0x40001C00, "TIM13" }, { 0x40002000, "TIM14" },
    { 0x40002800, "RTC/BKP" }, { 0x40002C00, "WWDG" }, { 0x40003000, "IWDG" },
    { 0x40003400, "I2S2ext" }, { 0x40003800, "SPI2" }, { 0x40003C00, "SPI3" },
    { 0x40004000, "I2S3ext" },
    { 0x40005400, "I2C1" }, { 0x40005800, "I2C2" }, { 0x40005C00, "I2C3" },
    { 0x40006400, "CAN1" }, { 0x40006800, "CAN2" }, { 0x40007000, "PWR" }, { 0x40007400, "DAC" },
    { 0x40010000, "TIM1" }, { 0x40010400, "TIM8" },
    { 0x40012000, "ADC" }, { 0x40012C00, "SDIO" },
    { 0x40013000, "SPI1" }, { 0x40013800, "SYSCFG" },
    { 0x40013C00, "EXTI" }, { 0x40014000, "TIM9" }, { 0x40014400, "TIM10" },
    { 0x40014800, "TIM11" },
    { 0x40020000, "GPIOA" }, { 0x40020400, "GPIOB" }, { 0x40020800, "GPIOC" },
    { 0x40020C00, "GPIOD" }, { 0x40021000, "GPIOE" }, { 0x40021400, "GPIOF" },
    { 0x40021800, "GPIOG" }, { 0x40021C00, "GPIOH" }, { 0x40022000, "GPIOI" },
    { 0x40023000, "CRC" }, { 0x40023800, "RCC" }, { 0x40023C00, "FLASH" },
    { 0x40024000, "BKPSRAM" }, { 0x40026000, "DMA1" }, { 0x40026400, "DMA2" },
    { 0x40028000, "ETH" }, { 0x40040000, "USB_OTG_HS" }, { 0x50000000, "USB_OTG_FS" },
    { 0x50060800, "RNG" }, { 0xA0000000, "FSMC" },
};

static const FbSoc soc_stm32f405 = {
    .name = "STM32F405",
    .cpu_type = ARM_CPU_TYPE_NAME("cortex-m4"),
    /* HSE 8 MHz, PLL M8 N336 P2 (iNav cmake/stm32.cmake, target/system_stm32f4xx.c) */
    .sysclk_hz = 168000000, .pclk1_hz = 42000000, .pclk2_hz = 84000000,
    .tim_clk_hz = { 84000000, 168000000 },
    .num_irq = 82,                  /* FPU_IRQn = 81 */
    .bitband = true,                /* StdPeriph writes RCC/PWR bits through 0x42000000 */
    .dbgmcu_idcode = 0x10076413,    /* DEV_ID 0x413 (F405/F407), REV_ID 0x1007 */
    .pwr_cr_reset = 0x00004000,     /* PWR_CR: VOS = 1 */
    .optcr_reset = 0x0FFFAAED,
    .flash_base = 0x08000000, .flash_size = 1 * MiB,
    .flash_alias_name = "flash.boot", .flash_alias = 0x00000000,    /* BOOT0 = 0: flash at 0 */
    .flash_sector = f405_flash_sector, .nsectors = ARRAY_SIZE(f405_flash_sector) - 1,
    .ram = { { "sram", 0x20000000, 128 * KiB },                     /* SRAM1 112K + SRAM2 16K */
             { "ccm", 0x10000000, 64 * KiB } },                     /* core-coupled, CPU only */
    .sysmem_base = 0x1FFF0000, .sysmem_size = 64 * KiB,
    .uid_addr = 0x1FFF7A10, .flashsize_addr = 0x1FFF7A22,
    .usart_model = FB_USART_F2XX, .usart = f405_usart, .nusart = ARRAY_SIZE(f405_usart),
    .spi_model = FB_SPI_F4, .spi2_base = 0x40003800,
    .spi_empty = f405_spi_empty, .nspi_empty = ARRAY_SIZE(f405_spi_empty),
    .tim = f405_tim, .ntim = ARRAY_SIZE(f405_tim),
    .i2c_model = FB_I2C_V1, .i2c_name = "i2c1", .i2c_base = 0x40005400,
    .i2c_ev_irq = 31, .i2c_er_irq = 32,
    .exti_irq = { 6, 7, 8, 9, 10, 23, 40 },
    .dma2_irq = { 56, 57, 58, 59, 60, 68, 69, 70 },
    .blocks = f405_blocks, .nblocks = ARRAY_SIZE(f405_blocks),
    .usb_hs_base = 0x40040000,
};

/* ---- SpeedyBee F405 WING (STM32F405, iNav target SPEEDYBEEF405WING) ----------------------- */

/* target.c timerHardware[]: S1 TIM4_CH2 PB7, S2 TIM4_CH1 PB6, S3 TIM3_CH3 PB0, S4 TIM3_CH4 PB1,
 * S5 TIM8_CH3 PC8, S6 TIM8_CH4 PC9, S7 TIM8_CH2N PB14, S8 TIM2_CH1 PA15, S9 TIM2_CH3 PB10,
 * S10 TIM2_CH4 PB11, S11 TIM12_CH2 PB15 (all TIM_USE_OUTPUT_AUTO: motor or servo by the mixer).
 * Indexes into f405_tim: 0 TIM1, 1 TIM2, 2 TIM3, 3 TIM4, 4 TIM8, 5 TIM12. AF (stm32f4xx_gpio.h
 * GPIO_AF_TIMx): TIM1/2 AF1, TIM3/4 AF2, TIM8 AF3, TIM12 AF9. */
static const FbOutput speedybee_f405_wing_servo[] = {
    { 3, 2, false, 1, 7, 2 }, { 3, 1, false, 1, 6, 2 }, { 2, 3, false, 1, 0, 2 },
    { 2, 4, false, 1, 1, 2 }, { 4, 3, false, 2, 8, 3 }, { 4, 4, false, 2, 9, 3 },
    { 4, 2, true, 1, 14, 3 }, { 1, 1, false, 0, 15, 1 }, { 1, 3, false, 1, 10, 1 },
    { 1, 4, false, 1, 11, 1 }, { 5, 2, false, 1, 15, 9 },
};

static const FbBoard board_speedybee_f405_wing = {
    .machine = "speedybee-f405-wing",
    .desc = "SpeedyBee F405 WING (STM32F405, Cortex-M4) for FlightBench",
    .soc = &soc_stm32f405,
    .imu_cs_port = 0, .imu_cs_pin = 4,          /* ICM42605_CS_PIN PA4 */
    .imu_int_port = -1, .imu_int_pin = -1,      /* no USE_EXTI: the GYRO task polls */
    .osd_cs_port = 1, .osd_cs_pin = 12,         /* MAX7456_CS_PIN PB12 on SPI2 */
    .servo = speedybee_f405_wing_servo, .nservo = ARRAY_SIZE(speedybee_f405_wing_servo),
    .motor = { 3, 2, false, 1, 7, 2 }, .motor_dshot = false,  /* S1: ONESHOT125 (iNav's default) */
    .crsf_default = -1,                         /* no receiver on the board */
    .crsf_uarts = 0x1F, .osd_uarts = 0x1F, .osd_poll = true, .uart_range = "uart1..uart5",
    .msp_uart = 5,                              /* USART6: MSP in targetConfiguration() */
    .cli_config = true,
    .cfg_off = 0xE0000, .cfg_len = 0x20000,     /* sector 11, __config_start (stm32_flash_f405xg.ld) */
    .baro = true, .baro_spl06 = true,           /* SPL06-001 on I2C1 at 0x76 (BARO_I2C_BUS) */
    .mag = true,                                /* MAG_I2C_BUS BUS_I2C1: a GPS module's compass */
    .gps_uarts = 0x1F,                          /* USART1..UART5 (USART3: the target's GPS port) */
    /* target.h ADC_CHANNEL_1_PIN PC0 = VBAT (ADC123_IN10), ADC_CHANNEL_2_PIN PC1 = CURRENT (IN11);
     * no VBAT_SCALE_DEFAULT: iNav's 1100 (sensors/battery.h, an 11:1 divider); CURRENT_METER_SCALE
     * 195 (0.1 mV/A: 19.5 mV/A), offset 0 */
    .adc = true, .vbat_divider = 11.0, .curr_mv_per_a = 19.5, .adc_vbat_ch = 10, .adc_curr_ch = 11,
    .idle_systick = true,                       /* iNav's scheduler reads micros(), not CYCCNT */
    .imu_align = FB_ALIGN_CW270,                /* target.h IMU_ICM42605_ALIGN CW270_DEG */
};

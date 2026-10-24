/* Host unit tests of the output gate (hw/arm/fb_outputs.c.inc) without QEMU: the GPIO and timer
 * register values iNav 9.1.0 leaves on the SpeedyBee F405 WING's pads with feature
 * PWM_OUTPUT_ENABLE on (IOConfigGPIOAF: MODER 10 + the timer's AF) and off (IOConfigGPIO
 * IOCFG_OUT_OD + IOLo: MODER 01), the channel enable (CCxE / CCxNE) and TIM1/TIM8's BDTR.MOE. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../hw/arm/fb_outputs.c.inc"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* one GPIO port: MODER, AFRL, AFRH */
typedef struct { uint32_t moder, afr[2]; } Port;

static void set_mode(Port *p, int pin, unsigned mode) { p->moder = (p->moder & ~(3u << 2 * pin)) | mode << 2 * pin; }
static void set_af(Port *p, int pin, unsigned af)
{
    uint32_t *r = &p->afr[pin >> 3];
    *r = (*r & ~(0xFu << 4 * (pin & 7))) | af << 4 * (pin & 7);
}
static bool pin_af(const Port *p, int pin, int af) { return fb_out_pin_af(p->moder, p->afr[0], p->afr[1], pin, af); }

int main(void)
{
    /* the pads: S1 PB7 TIM4 AF2 (AFRL), S7 PB14 TIM8_CH2N AF3 (AFRH), S11 PB15 TIM12 AF9 */
    Port b = { 0 };

    CHECK(!pin_af(&b, 7, 2), "reset state (input) carries no pulse");
    set_mode(&b, 7, 2);
    CHECK(!pin_af(&b, 7, 2), "AF mode with AF0 (system) is not TIM4");
    set_af(&b, 7, 2);
    CHECK(pin_af(&b, 7, 2), "PWM_OUTPUT_ENABLE: AF mode + AF2 = TIM4 on PB7");
    CHECK(!pin_af(&b, 6, 2), "PB6 untouched");
    set_mode(&b, 7, 1);
    CHECK(!pin_af(&b, 7, 2), "no PWM_OUTPUT_ENABLE: GPIO output (driven low) carries no pulse");
    set_mode(&b, 7, 3);
    CHECK(!pin_af(&b, 7, 2), "analog mode carries no pulse");

    set_mode(&b, 14, 2);
    set_af(&b, 14, 3);
    CHECK(pin_af(&b, 14, 3), "PB14 AF3 (AFRH) = TIM8");
    CHECK(!pin_af(&b, 14, 2), "PB14 AF3 is not AF2");
    set_mode(&b, 15, 2);
    set_af(&b, 15, 9);
    CHECK(pin_af(&b, 15, 9), "PB15 AF9 = TIM12 (the top nibble of AFRH)");
    CHECK(pin_af(&b, 14, 3), "PB14 unchanged by PB15");

    /* timer side: TIM4_CH2 (general purpose) */
    const uint32_t cen = FB_TIM_CR1_CEN, cc2e = 1u << 4, cc2ne = 1u << 6, moe = FB_TIM_BDTR_MOE;
    CHECK(fb_out_tim_on(cen, cc2e, 0, false, 2, false), "TIM4: CEN + CC2E");
    CHECK(!fb_out_tim_on(0, cc2e, 0, false, 2, false), "counter stopped");
    CHECK(!fb_out_tim_on(cen, 1u << 0, 0, false, 2, false), "CC1E does not enable CH2");
    CHECK(!fb_out_tim_on(cen, cc2ne, 0, false, 2, false), "CC2NE does not enable CH2");
    /* TIM8_CH2N (advanced): CC2NE and MOE */
    CHECK(fb_out_tim_on(cen, cc2ne, moe, true, 2, true), "TIM8_CH2N: CEN + CC2NE + MOE");
    CHECK(!fb_out_tim_on(cen, cc2ne, 0, true, 2, true), "TIM8 without MOE: outputs off");
    CHECK(!fb_out_tim_on(cen, cc2e, moe, true, 2, true), "CC2E does not enable CH2N");
    CHECK(fb_out_tim_on(cen, 1u << 12, moe, true, 4, false), "TIM8_CH4: CC4E + MOE");
    CHECK(fb_out_tim_on(cen, 1u << 12, 0, false, 4, false), "MOE only matters on TIM1/TIM8");

    printf("test_outputs: %d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}

/*
 * Whether a timer output reaches its pad (STM32F4/F7, RM0090 / RM0431), from register values only:
 * no QEMU, host-tested by core/qemu/tests/test_outputs.c.
 *
 * A board output (a servo or ESC pad) carries the timer's waveform only when
 *  - the pad's GPIO is in alternate-function mode (MODER = 10) with the channel's AF number
 *    (AFRL for pins 0..7, AFRH for 8..15): iNav without feature PWM_OUTPUT_ENABLE configures its
 *    output pads as GPIO outputs driven low (drivers/pwm_output.c pwmOutAllocOutput), and a real
 *    board then sends nothing whatever the timer does;
 *  - the counter runs (CR1.CEN) and the channel output is enabled (CCER CCxE, CCxNE for a
 *    complementary CHxN output);
 *  - on an advanced-control timer (TIM1/TIM8), the main output enable BDTR.MOE is set (with MOE
 *    clear both OCx and OCxN are off or at their idle level: no pulses).
 */

#define FB_GPIO_MODE_AF     2u
#define FB_TIM_CR1_CEN      (1u << 0)
#define FB_TIM_BDTR_MOE     (1u << 15)

/* the pad's GPIO routes alternate function af (0..15) to pin (0..15) */
static inline bool fb_out_pin_af(uint32_t moder, uint32_t afrl, uint32_t afrh, int pin, int af)
{
    uint32_t afr = pin < 8 ? afrl : afrh;
    return ((moder >> (2 * pin)) & 3) == FB_GPIO_MODE_AF &&
           ((afr >> (4 * (pin & 7))) & 0xF) == (uint32_t)af;
}

/* the timer drives channel ch (1..4), its CHxN output when n; adv: TIM1/TIM8 */
static inline bool fb_out_tim_on(uint32_t cr1, uint32_t ccer, uint32_t bdtr, bool adv, int ch, bool n)
{
    return (cr1 & FB_TIM_CR1_CEN) &&
           ((ccer >> (4 * (ch - 1) + (n ? 2 : 0))) & 1) &&
           (!adv || (bdtr & FB_TIM_BDTR_MOE));
}

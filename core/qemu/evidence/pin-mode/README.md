# Outputs gated on the pad's pin mode (2026-09-30)

**Finding** (`inav-defaults`, `core/qemu/README.md` gap note at the time): with iNav's feature
`PWM_OUTPUT_ENABLE` off the firmware configures its output pads as GPIO outputs driven low
(`drivers/pwm_output.c`, `pwmOutAllocOutput`), so a real board sends no pulses — but the machine read the
pulse width from the timer's CCR whatever the pad's GPIO mode, and the trainer flew with outputs disabled
in the configurator. The product principle: the sim never masks the board.

**Change** (branch `pin-mode`): a board output carries the timer's waveform only when the pad's GPIO is in
alternate-function mode with the channel's AF number (`MODER`, `AFRL/AFRH`), the counter runs (`CR1.CEN`),
the channel output is enabled (`CCER CCxE`, `CCxNE` for a CHxN pad) and, on TIM1/TIM8, `BDTR.MOE` is set.
`fb_outputs.c.inc` (register logic, host-tested by `core/qemu/tests/test_outputs.c`), `stm32_soc.c.inc`
(every board output now carries port/pin/AF: Nexus-XR SERVO 1..4 + MOTOR, SpeedyBee F405 WING S1..S11),
`nexus_xr.c` (`tim_out_on`, the DShot DMA path checks the pad too, `servo_sig` per physics tick),
`nexus_xr_ctl.c.inc` (`state.pwm_us` `null` and `state.no_signal` naming a wired output without a pulse;
`docs/protocol.md`). A servo without signal holds its last position (`actuators` unchanged); the motor
stops when the ESC's signal-loss timeout passes.

## Evidence: `pinmode_evidence.py` (this directory; log `pinmode-evidence-2026-09-30.log`)
Trainer bench (`sessions/trainer.json`) twice, own core on ports 31270..31274, build
`~/flightbench-spike/qemu-pm`. Per bench: a first start with an empty cfg-cache (script, save, ACC
calibration, save → `applied`), then a fresh start from the cached snapshot flying `flight.py --plan
plane-launch` (T0 = 20 s, to 54 s) with the state stream sampled at 5 Hz.

**N — bench line `feature -PWM_OUTPUT_ENABLE`** (`bench_cli` file, layer `bench`): the layers report the
deliberate conflict (`config: conflict: feature PWM_OUTPUT_ENABLE: [aircraft:models/trainer/inav.cli:172]
… then [bench:…/trainer-nopwm.cli:1] feature -PWM_OUTPUT_ENABLE: the later layer wins`), both lines are
sent in order, the firmware saves with the feature off. The firmware **arms** (event `armed` at
t = 21.07 s, 96 armed samples, mode ANGLE) — and in every armed sample `pwm_us` is all `null` and
`no_signal` = `[aileron_l, aileron_r, elevator, rudder, motor]`, motor throttle 0. The scenario's launch
(an env function, not the firmware) throws the plane from 0.6 m: a ballistic hop to 1.7 m, then it slides
54 m and stops (`on_ground: true` at t = 54 s, still armed). Nothing the firmware commands reaches a pad.

**P — the shipped trainer** (feature on, same plan): arms at the same t = 21.07 s, `no_signal` `[]`,
`pwm_us` on the wired pads (S3..S6 servos 1500/1500/1580/1500 µs, S12 motor 1525 µs at t = 38 s, motor
throttle 0.52), climbs to 28 m and 414 m downrange by t = 55 s. Same run as the `51d4a9212465` gate.

## Gates (`gates-2026-09-30.log`, `scripts/gates.py`, build `qemu-pm`, ports 31280..31284)
Host suites (devices, fdm, core/qemu/tests, test_wiring.py, py_compile) pass; equiv 266/266 (+ cli_diff 2);
RS7 `c5a2b9bb6c49` ×2 (8.1 host s per 10 virtual s); RS7-osd `233f9d6f4ec7` ×2; trainer `51d4a9212465` ×2;
trainer-osd `570a8c51503e` ×2 — **all hold**: with outputs enabled the gating is guest-invisible.
Windows (packaged core of main `9d361d0`, `gates-win-2026-09-30.log`): RS7 `6d8d378c099f` ×2, trainer `c66d93c7274e` ×2 — hold.

## Caveats
- Pin modes are read at the physics tick (1 ms) and the DShot frame: a pad toggled between ticks is not seen.
- The board tables carry the AF numbers by hand (F7: TIM1/2 AF1, TIM3 AF2; F4: TIM1/2 AF1, TIM3/4 AF2,
  TIM8 AF3, TIM12 AF9): a new board must fill port/pin/AF for every output (`docs/add-a-board`, later).
- `feature -PWM_OUTPUT_ENABLE` is the only firmware path exercised; an MCU in reset and a channel with
  `CCxE` clear are covered by the host test, not by a flight.

# fdm/ — flight dynamics models (C, emulator-independent)

```bash
make        # build/libfbfdm.a
make test   # the models against the claims of docs/fdm.md (host only, no QEMU)
```
The specification is `docs/fdm.md`: every model is written from it (first principles and the standard
references it names, no simulator's code), and every test here is a claim stated there. The library
uses `fb/json.h` from `../devices` (parameters, scenarios); the core compiles both source sets into one
machine. The tests also link the ESC/DShot models (`../devices/src/{json,esc,dshot}.c`).

| file | what |
|---|---|
| `include/fb/fdm.h`, `src/fdm.c` | the one interface every aircraft type implements: inputs by actuator name, the motor speed from the ESC model and the shaft load back, air and wind, step, state + a type block, IMU, events, the holder and the launch; the table-driven parameter loader (`fdm_params`: an unknown name, a wrong type or an out-of-range value names itself) |
| `include/fb/body.h`, `src/body.c` | the rigid body every model shares: integration, the ground that stops a helicopter and levels it with rates the IMU reads, the crash rule once per flight segment, the holder (a hand or a stand), return to the pad |
| `include/fb/rotor.h`, `src/rotor.c` | a rotor disc: blade-element thrust with momentum-theory inflow solved together, the blade stall as the lift limit, a guard in the vortex-ring region, the torque to spin it, the in-plane drag |
| `src/heli.c` | the helicopter (`fdm: "heli"`): swashplate read-back (rear, front-left, front-right), the first-order tip-path plane with hub moments, the tail rotor, fuselage drag, the ESC's load |
| `include/fb/plane.h`, `src/plane.c` | the fixed wing (`fdm: "plane"`): the linear-coefficient model with the stall blend, control and rate derivatives, a fixed-pitch prop on the ESC's rpm, wheels that roll forward only with rotation on the mains, the rail/hand launch; the coefficient functions are public for analytic checks |
| `include/fb/plane_scenario.h`, `src/plane_scenario.c` | environment scenarios: timed pad / hold / release / launch steps as JSON data (`models/<id>/env/*.json`), on the common interface (any type) |
| `include/fb/fdm_math.h`, `src/fdm_math.c` | vectors, quaternions, the actuator with a transport delay and a slew limit |

Tests: `tests/test_rotor.c` (the disc: thrust with collective, momentum consistency, climb/descent
damping, translational lift, the mirror image at negative collective), `tests/test_heli.c` (the ten
helicopter claims: hover collective and thrust = weight, terminal climb and descent, inverted hover and
climb, translational lift, cyclic and tail authority, the shaft load, the servo delay, the ground, determinism),
`tests/test_plane.c` (the ten plane claims: trim glide and full-throttle speed against the analytic values from
the same coefficient functions, control steps, stall and recovery, take-off and roll-out, the launch with
iNav's bungee rule, wind, the six calibration poses, the aileron mis-mix, determinism). The heli and plane
tests fly the models with a small stand-in for the firmware's loops (an attitude PD on the cyclic with a
velocity hold, an elevator pitch hold), because a bare helicopter is open-loop unstable.

Parameters: `models/<id>/aircraft.json` → `fdm_params`, the nested object of `docs/fdm.md` §2.8 (heli) and
§3.8 (plane). The machine hands it to `fb_fdm_new` as it is; a bench's session-check builds the model once
and reports a wrong parameter with its path (`fdm_params.rotor.radius_m: a number in 0.05..5`).

In the machine: `core/qemu/hw/arm/fb_fdm.c.inc` (both boards, any type): per 1 ms tick the servo pulses by
actuator name → `fb_fdm_set_input_us`, `fb_fdm_set_motor_rpm(fb_esc_rpm)` → `fb_fdm_step` →
`fb_esc_set_load(fb_fdm_load)` → the scenario → events → the IMU through the board's alignment;
`launch`, `constraint`, `env` and `reset aircraft` drive it (`docs/protocol.md`).

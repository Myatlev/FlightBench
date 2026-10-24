# RS7 — Goosky RS7 Ultra, 700-class electric helicopter

First aircraft of FlightBench: the owner's own 700 (RotorFlight on a RadioMaster Nexus-XR).

| file | what |
|---|---|
| `aircraft.json` | `fdm: "heli"`, the actuators (`swash_a`, `swash_b`, `swash_c` = the 120° CCPM plate, rear, front-left, front-right; `tail`; `motor`), the transmission (13T pinion, 116T main gear), the ESC (520 kV on 12S), the two calibration functions, and `fdm_params`: the model's parameters as `docs/fdm.md` §2.8 defines them (rotor and tail geometry from the manufacturer's figures, the owner's swash setup ±12° / ±12°, 5 kg, the estimates to be tuned by feel) |
| `qemu-profile.txt` | the firmware profile lines the core types after the board config (session `profile`) |
| `visual/rs7.tscn` | the 3D model the app moves by the FDM pose |

The flight model is FlightBench's own helicopter (`fdm/src/heli.c`, `docs/fdm.md` §2): a rotor disc by
blade-element theory with momentum inflow, a first-order tip-path plane with hub moments, a tail rotor, fuselage
drag. Its numbers come from physics, not from a tune: hovering 5 kg at 1800 rpm takes 3.0° of blade pitch and
740 W at the shaft; +3° climbs at 7.4 m/s; full collective climbs at 17 m/s upright and inverted alike.

Board profile (mixer, governor, servo geometry) comes from the firmware's own config, not from here.
Lessons carried over from the earlier SITL bench: never copy servo reverse flags from the real board into a
model; the model does not reproduce linkage geometry.

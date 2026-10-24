# FlightBench flight dynamics (FDM) — specification

Status: **draft, owner's answers of 2026-10-24 folded in (swash layout, ±12°/±12°, 5 kg, head stiffness and Lock number to be tuned by feel, the trainer stays abstract).** This document is the source of truth for `fdm/`: the models are written
from it, and every test in `fdm/tests/` is a claim stated here. The physics is written from first principles and
standard references (momentum and blade-element theory for rotors, the linear-coefficient model for fixed wings); no
simulator's source code is a source for anything below.

References (books, not code): J. G. Leishman, *Principles of Helicopter Aerodynamics* (momentum / blade-element theory,
inflow, rotor torque); R. W. Prouty, *Helicopter Performance, Stability and Control* (flapping, hub moments);
B. Mettler, *Identification Modeling and Characteristics of Miniature Rotorcraft* (the first-order tip-path-plane
model that fits small helicopters); R. W. Beard, T. W. McLain, *Small Unmanned Aircraft* (the fixed-wing
coefficient model, stall blending); B. L. Stevens, F. L. Lewis, *Aircraft Control and Simulation* (6-DoF equations).

## 1. What an FDM is here

An FDM is one C object per aircraft type behind one typed interface. The core steps it every 1 ms of virtual time
with the flight controller's actuator outputs; it answers with what an IMU rigidly mounted on the airframe would
measure, the pose for the 3D view and the Signals window, and events. It is deterministic: the same parameters and
the same inputs give bit-identical state (double precision, fixed step, no host time, no randomness; the sensor
layer adds noise, not the FDM).

Frames and units (all models): NED world frame (x north, y east, z down; the pad is the origin, z = 0 is the
ground under the model's reference point), FRD body frame (x forward, y right, z down), attitude as a unit
quaternion body→NED, SI units (m, m/s, rad, rad/s, N, N·m, kg, kg·m²), rotor speeds in rpm at the interface.
Angles in parameter files are degrees; the loader converts.

Common state (`fb_fdm_state_t`): `t`, `pos_ned[3]`, `vel_ned[3]`, `quat[4]`, `rates[3]` (body), `accel_body[3]`
(specific force, body: at rest on the ground `(0, 0, -g)`), `on_ground`, `crashes`, `crash_info[3]`, plus a
type-specific block (rotor speeds, thrust, collective; airspeed, α, β, surface positions).

Common inputs: actuator values by name (µs, the firmware's outputs after the wiring's reversal), motor speed(s)
from the ESC model (`set_motor_rpm`), air density, wind (NED). Common outputs for the drivetrain: shaft load 0..1 for
the ESC model (`load`). Common environment API: `reset`, `return_to_pad`, `hold` / `hold_quat` / `release`
(a hand or a stand), `launch` (plane), events (pose reached, launch start/release, crash, mis-mix).

The ESC owns the motor: throttle → current → rpm with the shaft load the FDM reports. The FDM never models the
motor; with no ESC bound the rotor/prop speed follows the throttle through a first-order lag (`tau_s`), a stand-in,
not a motor model.

Ground model (all types, kept from today's implementation — our own): contact when the reference point reaches
z = 0; the model is held (velocity zero) and rolled/pitched level in 0.25 s with consistent body rates (the attitude
never jumps, the IMU can follow); a contact beyond the type's limits counts as a crash once per flight segment
(airborne > 0.2 s re-arms it). Thrust is never blocked by a crash.

Servos (all types, ours): a transport delay (whole steps, `servo_delay_ms`) and a slew limit (`servo_speed_s60`,
seconds per 60° of deflection; 0 = instant).

## 2. Helicopter (`fdm: "heli"`)

### 2.1 Model summary
A single main rotor with a tail rotor, a rigid fuselage, flybarless. The main rotor is a disc with a first-order
tip-path-plane (TPP): the swashplate commands collective and cyclic pitch; thrust comes from blade-element theory with
momentum-theory inflow (so climb and descent damping, translational lift and the power/torque to spin the head are
consequences, not coefficients); cyclic tilts the TPP with a lag, and the tilted TPP acts on the body through the
hub moment (flapping stiffness + thrust offset). The tail rotor is the same blade-element thrust along the body
y axis at the tail arm. The fuselage adds quadratic drag. Rotor speed comes from the ESC model (the firmware's
governor closes its loop through a real ESC + motor model and the rotor's real torque load).

### 2.2 Inputs
`swash_a`, `swash_b`, `swash_c` (µs), `tail` (µs), motor rpm from the ESC. The swashplate is a 120° CCPM with one
servo behind the mast and two in front at the sides (the RS7's layout; `swash.order` names the actuators in the
order rear, front-left, front-right, so any other 120° layout is a permutation). The firmware mixes collective and
cyclic into three servo positions; the model reads the plate's geometry back, which does not depend on the
firmware's mixer. With servo positions `s_i = (us_i − centre_i) / travel_i` in −1..1 (+ = the servo lifts its
point of the plate; `servo_travel_us` is the µs for full deflection) and `s_r`, `s_l`, `s_R` the rear, front-left and
front-right servos:

```
collective  = (s_r + s_l + s_R) / 3                      · collective_max_deg
pitch_cyc   = ((s_l + s_R) / 2 − s_r) · (2/3)            · cyclic_max_deg    (+ = plate front high: TPP back, nose up)
roll_cyc    = (s_l − s_R) / (2 · sin 60°)                · cyclic_max_deg    (+ = plate right low: TPP right, roll right)
tail_pitch  = s_tail                                      · tail_pitch_max_deg
```
`collective_max_deg` / `cyclic_max_deg` are the blade pitch the plate gives at full servo travel (`servo_travel_us`
from centre, all three servos for collective, the cyclic pattern above for cyclic): the owner's swash setup is
±12° collective and ±12° cyclic at the firmware's full command. How many µs the firmware outputs at its full
command is measured on the bench (the live check logs the servo outputs at the collective limits); the loader's
`servo_travel_us` is set from that measurement, so the degrees above are the pitch-gauge numbers as read. The
firmware's own ranges and limits stay in the firmware.

### 2.3 Main rotor thrust and torque
Geometry: radius `R`, chord `c`, `blades` N_b, lift-curve slope `a` (per rad; 5.7 for a thin airfoil at these
Reynolds numbers is the default), profile drag `cd0`, tip-loss factor `B` (0.97), induced-power factor `kappa`
(1.15). Solidity σ = N_b c / (π R). Disc area A = π R². Tip speed V_t = Ω R with Ω = rpm · 2π/60.

Flow through the disc in the TPP-normal direction: the body-frame air velocity `v_a = v_body − R^T wind`; the
component along the TPP normal `v_n` (positive = air flowing up through the disc, i.e. descent) and the in-plane
speed `v_p`. Non-dimensional: climb ratio λ_c = −v_n / V_t (positive in climb), advance ratio μ = v_p / V_t.

Blade-element thrust (uniform inflow, untwisted blades, linear lift up to the blade's stall: the angle of attack
at 0.75 R, θ0 − λ/0.75, is limited to ±`stall_deg`, so a steep descent or a wild collective cannot make the blades
lift without bound; there is no post-stall model):
```
C_T = (σ a / 2) · ( θ0 (B³/3 + μ² B/2) − λ B² / 2 )         λ = λ_c + λ_i, clamped by the stall
```
Momentum-theory inflow (Glauert, valid in climb, hover and forward flight):
```
λ_i = C_T / ( 2 √( μ² + λ² ) )
```
The two are solved together each step by a relaxed fixed-point iteration on λ_i (warm-started from the previous
step, ≤ 40 iterations, relative tolerance 1e-9). In the vortex-ring region of descent momentum theory has no
solution; the denominator √(μ² + λ²) is floored at the hover inflow λ_h = √(|C_T|/2), so the induced velocity never
exceeds its hover value there and thrust stays continuous (an engineering approximation; the firmware never flies a
vertical autorotation on the bench). Negative collective gives negative C_T and the same equations with the signs of
λ_i and λ_h following C_T, so inverted flight is the same physics upside down.

Thrust `T = C_T ρ A V_t²` along the TPP normal (body z negative = up when the TPP is level). Torque to turn the
rotor (what the motor supplies and the fuselage feels as yaw reaction, sign by `rotor_direction`):
```
C_Q = kappa · C_T · λ_i + C_T · λ_c + (σ cd0 / 8) · (1 + 4.6 μ²)      Q = C_Q ρ A V_t² R
```
Shaft load for the ESC: `load = Q (1 + tail_power_fraction) / rated_torque_nm`, with `rotor.rated_torque_nm` the
motor's torque at its rated current seen at the rotor (`60 / (2π kv) · I_max · gear`: 32 N·m for the RS7's 520 kV,
200 A, 8.9:1), so hover sits at ≈ 0.13 of rated, ≈ 26 A at 50 V.

The in-plane rotor drag (H-force) is taken as the profile part only: `H = (σ cd0 / 4) ρ A V_t² μ` opposing `v_p`.

### 2.4 Tip-path plane and hub moments
Longitudinal and lateral TPP tilt `a1` (positive = TPP tilted back, nose-up thrust) and `b1` (positive = tilted
right) follow the cyclic command with the rotor's flapping time constant and feel the body rates (the TPP lags the
body):
```
τ_f  = 16 / (γ Ω)                              γ = lock_number (default 4.5 for a 700 FBL rotor)
da1/dt = ( −a1 + pitch_cyc_rad ) / τ_f  − q
db1/dt = ( −b1 + roll_cyc_rad  ) / τ_f  − p
```
Hub moments and thrust tilt: with hub height `h` above the CG and flapping stiffness `k_beta` (N·m/rad, the
flybarless head's blade-grip / dampener stiffness; a large value for a stiff 3D head),
```
M_pitch = (k_beta + T h) · a1  (nose up)      M_roll = (k_beta + T h) · b1  (right)
F_body  = T · ( −sin a1, sin b1, −cos a1 cos b1 )        (the thrust vector follows the TPP normal)
```
Cross-coupling of the flapping axes is left out in v1 (`a1`/`b1` decoupled); the firmware's own cyclic
feed-forward and PIDs are what fly the model, not a tuned coupling.

### 2.5 Tail rotor
Same blade-element thrust with hover inflow in closed form (no iteration: the tail's advance ratio is small):
```
C_Tt = (σ_t a / 2) ( θ_t B³/3 − λ_t B²/2 ),   λ_t = √(|C_Tt| / 2) · sign(C_Tt)   (solved by 4 fixed-point passes)
T_t  = C_Tt ρ A_t V_tt²,   V_tt = Ω · tail_ratio · R_t
```
Positive tail pitch pushes the tail left, i.e. yaws the nose right: force `(0, −T_t, 0)` at
`(−tail_arm, 0, −tail_height)` → yaw moment `N = T_t · tail_arm`, roll moment `−T_t · tail_height`, plus the main
rotor's torque reaction (`−Q` about z for a clockwise rotor seen from above, `+Q` counter-clockwise). The tail's own
power is a fraction of the main rotor's in the shaft load (`tail.power_fraction`, 0.08).

### 2.6 Fuselage
Quadratic drag per body axis with reference areas `drag_area_m2[3]` (`C_D · S` for x, y, z):
`F_i = −½ ρ |v_a,i| v_a,i · drag_area_i`. No fuselage lift, no fin: the tail rotor and the firmware's yaw loop hold
the heading, as on the real 700.

### 2.7 Equations of motion
Forces in the body frame: rotor thrust (tilted), H-force, tail thrust, fuselage drag; gravity added in NED.
Moments: hub moments, tail, torque reaction, plus the rotor's in-plane damping through the TPP rate terms above.
Rigid body with the diagonal inertia `inertia_kgm2[3]`:
```
dω/dt = I⁻¹ ( M − ω × (I ω) )
dq/dt = ½ q ⊗ (0, ω)           (normalised each step)
dv_ned/dt = R(q) F_body / m + (0, 0, g)
dp/dt = v_ned
```
Semi-implicit Euler at 1 ms (the rates and the TPP are integrated first, then the attitude, then velocity and
position). Specific force for the IMU = `F_body / m` (gravity excluded; on the ground the holder's reaction is
`R^T (0, 0, −g)`).

### 2.8 Parameters (`fdm_params`, helicopter)
| name | unit | RS7 Ultra default | from |
|---|---|---|---|
| `mass_kg` | kg | 5.0 | airframe 3.7 kg (manufacturer) + the 12S pack; the owner's figure |
| `inertia_kgm2` | kg·m² | [0.12, 0.35, 0.30] | estimate for a 700 (mass distribution along x; refine from a swing test) |
| `rotor.radius_m` | m | 0.804 | rotor diameter 1608 mm (manufacturer) |
| `rotor.chord_m` | m | 0.060 | 700-class main blade |
| `rotor.blades` | — | 2 | |
| `rotor.lift_slope` | 1/rad | 5.7 | thin-airfoil value |
| `rotor.cd0` | — | 0.011 | symmetric RC blade section |
| `rotor.tip_loss` | — | 0.97 | standard |
| `rotor.kappa` | — | 1.15 | standard induced-power factor |
| `rotor.stall_deg`, `tail.stall_deg` | deg | 14 | blade section stall (lift limit) |
| `rotor.lock_number` | — | 4.5 | FBL 700 (flapping lag ≈ 19 ms at 1800 rpm) |
| `rotor.hub_stiffness_nm_per_rad` | N·m/rad | 120 | stiff 3D head; tune from the firmware's cyclic feel |
| `rotor.hub_height_m` | m | 0.22 | hub above the CG |
| `rotor.direction` | — | `cw` | seen from above |
| `rotor.nominal_rpm` | rpm | 1800 | the owner's `gov_headspeed` (the stand-in motor's top speed; the ESC owns the real one) |
| `rotor.rated_torque_nm` | N·m | 32 | motor torque at rated current, at the rotor (see §2.3) |
| `tail.radius_m` | m | 0.1475 | tail rotor diameter 295 mm (manufacturer) |
| `tail.chord_m` | m | 0.030 | 105 mm tail blades |
| `tail.blades` | — | 2 | |
| `tail.ratio` | — | 5.0 | tail drive 5.0:1 (manufacturer) |
| `tail.arm_m` | m | 0.85 | hub to tail rotor |
| `tail.height_m` | m | 0.05 | tail rotor above the CG |
| `tail.pitch_max_deg` | deg | 12 | at full servo travel |
| `tail.power_fraction` | — | 0.08 | tail power as a fraction of the main rotor's, in the shaft load |
| `swash.collective_max_deg` | deg | 12 | the owner's pitch-gauge setup, both ways |
| `swash.cyclic_max_deg` | deg | 12 | the owner's setup, both ways |
| `swash.order` | — | `["swash_a","swash_b","swash_c"]` | rear, front-left, front-right (checked against the preset's wiring) |
| `drag_area_m2` | m² | [0.045, 0.09, 0.07] | C_D·S per axis (estimate) |
| `servo.swash_travel_us`, `servo.tail_travel_us` | µs | 500, 270 | the servos' real travel; the tail's from the bench |
| `servo.delay_ms`, `servo.speed_s60` | ms, s | 8, 0.09 | |
| `standin_tau_s` | s | 3 | the stand-in motor's lag when no ESC is bound |
| `crash` | | roll/pitch > 15°, sink > 1 m/s, drift > 2 m/s | as today |

Computed by the implementation (hover, 5 kg, 1800 rpm; `fdm/tests/test_rotor.c`): collective 3.0° by blade-element
theory (real pitch gauges read 5–6° on a 700: blade twist under load and the gauge's position; `collective_max_deg`
absorbs the scale, the test asserts a range), rotor torque 3.9 N·m → 740 W at the shaft, hover load 0.13 of rated,
tail pitch 4.8° with a 5.3° hover lean, +3° of collective → 7.4 m/s terminal climb, full collective → 17 m/s upright and
inverted alike. Plausible for a 700.

### 2.9 Claims the tests make (heli)
1. Hover: with the ESC at nominal rpm there is a collective in 2.5..7° at which thrust = weight within 1 %, found by
   the test by bisection; at that collective, level, the model hovers (|v| < 0.3 m/s after 5 s with a trivial
   attitude hold in the test).
2. Climb/descent damping: +3° above hover collective gives a terminal climb rate in 3..12 m/s; −3° a terminal descent
   in the same range; no runaway for 60 s either way.
3. Inverted: rolled to 180°, collective of the same magnitude negative, the model holds height within ±2 m for 10 s
   and climbs at full negative collective at a rate within 20 % of the upright climb.
4. Translational lift: at 10 m/s forward flight the collective for level flight is lower than in hover.
5. Cyclic: a 1 s pitch-cyclic step produces a pitch rate of the commanded sign within 0.1 s and a steady rate in
   30..300 °/s; roll the same; no cross-axis rate above 20 % of the commanded axis.
6. Tail: full tail pitch from hover yields a yaw acceleration of the commanded sign > 200 °/s²; with zero tail pitch
   the torque reaction turns the fuselage against the rotor direction.
7. Torque: at hover the shaft load is in 0.3..0.8 of rated; at full positive collective at nominal rpm it exceeds
   hover's by at least 2×; the ESC model's rpm droop under that load is < 8 % with the stock governor model.
8. Servo delay: a swash step appears in the thrust after `servo_delay_ms` ± 1 ms.
9. Ground: as today (level-in-0.25 s, crash rule, no jumps in the IMU).
10. Deterministic: two runs, bit-identical state logs.

## 3. Fixed wing (`fdm: "plane"`)

### 3.1 Model summary
Rigid aircraft with the standard linear-coefficient aerodynamic model in the stability/body frame, a lift curve
that blends into a flat plate past stall, induced drag from the lift, control derivatives for ailerons (L/R, mixed
to one roll command), elevator and rudder, rate derivatives for damping, a propeller thrust model from the motor
rpm with the airspeed roll-off, wind, and a wheels-on-ground model (rolls forward only).

### 3.2 Air data
`v_a = v_body − R^T wind`, airspeed V = |v_a|, angle of attack α = atan2(v_a,z, v_a,x), sideslip
β = asin(v_a,y / V), dynamic pressure q̄ = ½ ρ V². Below V = 0.5 m/s the aerodynamic forces and moments are zero.

### 3.3 Lift, drag, side force
Linear lift blended into a flat plate with a sigmoid of width `stall_sharpness` around `alpha_stall`:
```
s(α)  = ( 1 + e^(−k(α−α_s)) + e^(k(α+α_s)) ) / ( (1 + e^(−k(α−α_s))) (1 + e^(k(α+α_s))) )   k = stall_sharpness
C_L   = (1 − s) (cl0 + cl_alpha α) + s · 2 sign(α) sin²α cos α
C_D   = cd0 + (cl0 + cl_alpha α)² / (π · oswald · AR)        AR = b² / S
C_Y   = cy_beta β + cy_p (b p / 2V) + cy_r (b r / 2V) + cy_aileron δa + cy_rudder δr
```
Lift and drag act in the wind axes and are rotated into the body frame by α: `F_x = −D cos α + L sin α`,
`F_z = −D sin α − L cos α` (times q̄ S), with the pitch-rate and elevator contributions
`cl_q (c q / 2V)`, `cl_elevator δe`, `cd_elevator |δe|` added to C_L / C_D before the rotation.

### 3.4 Moments
```
l = q̄ S b ( cl_beta β + cl_p (b p / 2V) + cl_r (b r / 2V) + cl_aileron δa + cl_rudder δr )
m = q̄ S c ( cm0 + cm_alpha α + cm_q (c q / 2V) + cm_elevator δe )
n = q̄ S b ( cn_beta β + cn_p (b p / 2V) + cn_r (b r / 2V) + cn_aileron δa + cn_rudder δr )
```
plus `r_cp × F_aero` for the aerodynamic centre offset `cp_offset_m` from the CG (nose-heavy trim comes from
`cm0` and this offset, not from a hidden constant).

### 3.5 Controls
Surface positions −1..1 after the servo delay/slew. Roll command `δa = (ail_l − ail_r) / 2` (an aileron's +1 =
trailing edge down; both the same way is a mis-mix: event `AILERON_MIX`, roll command 0), `δe` = elevator (+1 =
trailing edge up, nose up), `δr` = rudder (+1 = trailing edge right, nose right). Deflection in radians for the
coefficients = command · `surface_deflection_deg`.

### 3.6 Propeller and motor
Thrust from the motor rpm `n` (rev/s) with the airspeed roll-off of a fixed-pitch prop:
```
T = ct ρ n² D⁴ · ( 1 − V / V_pitch ),   V_pitch = pitch · n     (clamped at 0 for V > V_pitch)
```
or `kt` N/rpm² when given (a measured static curve). Shaft load for the ESC = T / T_static(rpm_max). The prop's
torque reaction on the body is `−T · prop_torque_factor` about x (small, 0 by default). Thrust acts along body x at
`thrust_offset_m` from the CG (z offset gives the familiar pitch-with-throttle).

### 3.7 Equations of motion, ground, launch
As the helicopter (§2.7). Ground: when the wheels (z = 0) touch, the forward-only roll: body-y velocity set to 0,
backward velocity set to 0, no sinking; rolling friction `−ground_friction · v_x`; wings level and the heading kept;
below 5 m/s the nose is held level, above it the pitch moment rotates the nose on the main wheels against the weight on
the nose wheel (`mass · g · gear_arm_m`) between level and 15° (a tail strike beyond); contact at |roll| > 30°, pitch < −20° or sink > 2.5 m/s is a crash.
Hold / pose / launch (the environment API and `env/*.json` scenarios) stay as today: they are constraints, not
aerodynamics.

### 3.8 Parameters (`fdm_params`, plane) and the trainer's values
The trainer is "a 1.9 m electric trainer", not any particular product; its numbers are estimates from its geometry
with the standard formulas, to be tuned on the bench (iNav must fly it with its defaults; that is the acceptance).
| name | unit | trainer | how |
|---|---|---|---|
| `mass_kg` | kg | 2.0 | |
| `inertia_kgm2` | kg·m² | [0.12, 0.09, 0.19] | slender-body estimates |
| `wing.area_m2`, `wing.span_m`, `wing.chord_m` | | 0.45, 1.88, 0.24 | AR = 7.85 |
| `aero.cl0`, `aero.cl_alpha` | —, 1/rad | 0.30, 5.0 | cambered trainer wing; 2π·AR/(AR+2) |
| `aero.alpha_stall_deg`, `aero.stall_sharpness` | deg, — | 15, 40 | |
| `aero.cd0`, `aero.oswald` | — | 0.045, 0.85 | clean trainer with gear |
| `aero.cl_q`, `aero.cl_elevator`, `aero.cd_elevator` | 1/rad | 3.0, 0.25, 0.02 | tail volume ≈ 0.5 |
| `aero.cm0`, `aero.cm_alpha`, `aero.cm_q`, `aero.cm_elevator` | 1/rad | 0.04, −0.8, −12, −1.1 | static margin ≈ 15 % |
| `aero.cy_beta`, `aero.cy_rudder` | 1/rad | −0.6, 0.15 | |
| `aero.cl_beta`, `aero.cl_p`, `aero.cl_r`, `aero.cl_aileron`, `aero.cl_rudder` | 1/rad | −0.08, −0.45, 0.12, 0.18, 0.01 | dihedral 3°, strip theory |
| `aero.cn_beta`, `aero.cn_p`, `aero.cn_r`, `aero.cn_aileron`, `aero.cn_rudder` | 1/rad | 0.09, −0.03, −0.12, −0.01, −0.07 | fin volume ≈ 0.04 |
| `cp_offset_m` | m | [−0.02, 0, −0.04] | aerodynamic centre vs CG |
| `prop.diameter_in`, `prop.pitch_in`, `prop.ct`, `prop.kt` | | 11, 7, 0.10, null | as today's powertrain sizing |
| `thrust_offset_m`, `prop_torque_factor` | | [0.3, 0, 0], 0 | |
| `rpm_max` | rpm | 14800 | 1000 kV · 14.8 V |
| `surface_deflection_deg`, `servo_delay_ms`, `servo_speed_s60` | | 20, 10, 0.1 | |
| `servo_centre_us`, `servo_travel_us` | µs | 1500, 500 | |
| `ground_friction`, `gear_height_m`, `gear_arm_m` | 1/s, m, m | 0.3, 0.1, 0.06 | gear_arm: the CG ahead of the main wheels |

### 3.9 Claims the tests make (plane)
1. Trim: the stick-fixed glide speed and L/D agree with the analytic values from the coefficients within 5 %.
2. Static thrust/weight at `rpm_max` > 1.2; full-throttle level speed within 5 % of the analytic value.
3. Step responses: elevator / aileron / rudder steps give rates of the right sign and plausible magnitude.
4. Stall: lift collapses past `alpha_stall`; a stalled model pitches down and recovers (cm_alpha < 0).
5. Ground: rolls from rest to lift-off speed under thrust, climbs after lift-off, never rolls sideways or
   backwards, rolls out and stops.
6. Launch: the ramp's IMU x = a + g sin(pitch); iNav's bungee condition (18.63 m/s² for 40 ms) holds for the default
   profile; a nose-down launch stays above ground; it climbs once the motor runs.
7. Wind: a wind step changes airspeed, not ground speed; the trim airspeed is kept in wind.
8. Poses: all six iNav calibration positions are reached and held still; IMU rates follow the rotation.
9. Mis-mix: same-sign ailerons warn and give no roll.
10. Deterministic.

## 4. Interface (`fdm/include/fb/fdm.h`)
```c
typedef enum { FB_FDM_HELI, FB_FDM_PLANE } fb_fdm_kind_t;
fb_fdm_t *fb_fdm_new(fb_fdm_kind_t kind, const char *params_json, double dt_s, char *err, size_t errlen);
void fb_fdm_free(fb_fdm_t *);
void fb_fdm_reset(fb_fdm_t *);
void fb_fdm_return_to_pad(fb_fdm_t *);
int  fb_fdm_input_index(const fb_fdm_t *, const char *actuator);          /* by name, -1 unknown */
void fb_fdm_set_input_us(fb_fdm_t *, int index, uint16_t us);
void fb_fdm_set_motor_rpm(fb_fdm_t *, int motor, double rpm);              /* < 0: no ESC bound */
double fb_fdm_load(const fb_fdm_t *, int motor);                           /* 0..1 for the ESC */
void fb_fdm_set_air(fb_fdm_t *, double rho);   void fb_fdm_set_wind(fb_fdm_t *, const double ned[3]);
void fb_fdm_step(fb_fdm_t *);
void fb_fdm_state(const fb_fdm_t *, fb_fdm_state_t *out);                  /* common block */
const void *fb_fdm_state_ext(const fb_fdm_t *, size_t *size);              /* the type's block */
void fb_fdm_imu(const fb_fdm_t *, float gyro_dps[3], float accel_g[3]);
void fb_fdm_euler(const fb_fdm_t *, double *roll, double *pitch, double *yaw);
/* environment / constraints */
void fb_fdm_hold(fb_fdm_t *, double roll, double pitch, double yaw, double rotate_s);
void fb_fdm_hold_quat(fb_fdm_t *, const double q[4], double rotate_s);
void fb_fdm_release(fb_fdm_t *);
int  fb_fdm_launch(fb_fdm_t *, double speed_mps, double duration_s, double heading, double pitch);
bool fb_fdm_poll_event(fb_fdm_t *, fb_fdm_event_t *ev);
```
The machine glue (`fb_heli` / `fb_plane` in `core/qemu/hw/arm/`) talks to this interface only; the scenario runner
(`plane_scenario.c`, ours) moves onto it unchanged in behaviour. `aircraft.json` → `fdm_params` is the nested
object of §2.8 / §3.8 (the loader rejects unknown names and reports the first error with its path).

## 5. What left the repository with this rewrite (done 2026-10-24)
The two ported models and their headers (replaced by `fdm/src/heli.c`, `fdm/src/plane.c` behind `fb/fdm.h`), the
SITL-era Python model and stand tools of the RS7 (kept in the owner's lab repository), the trainer's borrowed
parameter set (replaced by estimates from its own geometry), and every mention of the third-party simulator as a
source in the READMEs, the decisions and the spike document. ArduPilot stays in one place: the roadmap line
"firmware targets planned: … ArduPilot".

## 6. Acceptance
- `make -C fdm test`: every claim above, plus the determinism gate.
- The RS7 · First flight preset: stock RotorFlight 4.6.0 with the preset's profile spools up, lifts off from the
  keyboard (the live check's two PASS lines) and holds a hover and an inverted hover under the pilot script, with
  the governor holding 1800 ± 50 rpm.
- The Trainer presets: stock iNav 9.1.0 calibrates, launches and climbs under NAV LAUNCH with its defaults.
- Evidence regenerated (`core/qemu/evidence/`, `app/evidence/`), hashes of the determinism gate re-baselined.

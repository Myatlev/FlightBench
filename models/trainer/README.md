# Trainer — electric trainer plane (a 1.9 m placeholder, no particular product)

Second aircraft of FlightBench, the first fixed-wing: aileron L/R, elevator, rudder, one motor +
ESC, flying stock iNav 9.1.0 on the SpeedyBee F405 WING (docs/spike-inav-plane-2026-09-30.md).

| file | what |
|---|---|
| `aircraft.json` | `fdm: "plane"`, actuators, the ESC, `fdm_params` (the model's nested object, `docs/fdm.md` §3.8: wing, aero, prop, servos, ground), the env scenarios |
| `inav.cli` | the aircraft part of the iNav configuration: the iNav Configurator's "Airplane with a Tail" defaults and mixer preset (with its two flags), servo reversals, `feature PWM_OUTPUT_ENABLE`, ARM/ANGLE/NAV LAUNCH switches (the board part is the board script; the GPS's lines come with the wired `ublox`); below, "Configuration" |
| `env/launch.json` | default environment: on the pad, rotated onto the launch attitude, launched by the `launch` command (50 km/h in 0.5 s at 10°) |
| `env/ground-takeoff.json` | on a flat runway, free to roll |
| `env/acc-calibration.json` | the six still poses of iNav's accelerometer calibration, with a `pose` event each (the `calibrate_acc` function's `scenario`: the first start runs it once per firmware + script, below) |

Scenario schema: `docs/protocol.md` → `env.scenario`, `fdm/include/fb/plane_scenario.h`.

## Numbers
- **Airframe:** an abstract 1.9 m electric trainer (2.0 kg, 0.45 m², 1.88 m span, AR 7.85). The aerodynamic
  coefficients are estimates from that geometry with the standard formulas (`docs/fdm.md` §3.8 says how each was
  obtained: lift slope 2π·AR/(AR+2), a cambered-wing CL0, tail and fin volumes for the pitch and yaw derivatives);
  stick-fixed it glides at 13.4 m/s with L/D 7.5 (`fdm/tests/test_plane.c`). They are to be tuned on the bench: the
  acceptance is that stock iNav flies it with its defaults.
- **Powertrain (placeholder):** 1000 kV on 4S (14.8 V), 11×7 prop, static thrust coefficient 0.10 → 45 N static at
  full throttle, 14 800 rpm (T/W 2.3); full-throttle level flight at about 31 m/s. Replace `prop.ct` or set
  `prop.kt` (N/rpm²) from a measured thrust curve.
- **Servos:** 10 ms transport delay, 0.1 s/60°, ±20° at full travel. **Ground:** rolling friction 0.3 1/s, the CG
  0.06 m ahead of the main wheels (the nose wheel's hold-down: rotation needs the elevator).

## On the bench (phase 3)
`core/qemu/sessions/trainer.json` (core form) and `presets/inav-trainer-launch.json` (app form): the
speedybee-f405-wing machine with the stock iNav 9.1.0 release hex, CRSF on uart1, the ESC on S1
(STANDARD PWM since the configurator defaults, 1000..2000 µs; the pulse width read from the timer's CCR), servos S3..S6 = aileron_l, aileron_r,
elevator, rudder. With `fdm: "plane"` the machine runs `fdm/src/plane.c` (`core/qemu/hw/arm/fb_fdm.c.inc`).

- **Servo order.** iNav gives the motor S1 and, because a timer cannot mix motor and servo, its TIM4
  twin S2; the servos follow on S3.. in servo-index order from the lowest index the mixer uses
  (`flight/servos.c:294`: output = servo index - minServoIndex). `smix` servos 1..4 therefore land on
  S3..S6, as wired.
- **Configuration.** The core types `boards/speedybee-f405-wing/defaults.cli` + `inav.cli` into the CLI
  on USART6 and saves (the firmware reboots).
- **Environment functions** (`functions` in `aircraft.json`, `docs/protocol.md` `function`; the app
  lists them from the manifest): `calibrate_acc` (the poses below; `auto_first_start`), `calibrate_mag`
  (iNav's compass calibration with the core's sweep; `auto_first_start`, when a compass is wired), `calibrate_gyro`
  (held level and still, an MCU reset: iNav calibrates the gyro at boot; the core reports when its
  `CAL` blocker clears), `launch` (the hand launch, 50 km/h in 0.5 s at 10°, as `env/launch.json`).
- **ACC calibration** (`calibrate_acc`, `auto_first_start`): after that save the core runs
  `env/acc-calibration.json` on the FDM; at each pose event with an iNav position `index` it sends
  `MSP_ACC_CALIBRATION` and polls `MSP_CALIBRATION_DATA` until the firmware has that position (≈0.53 s:
  iNav samples 500 ms), then enters the CLI again, runs `diff all` (kept with the snapshot, the `diff`
  message) and `save`s. The config sector with the calibration is the snapshot for this firmware +
  script + the automatic functions (their scenario), kept in memory and on disk (`-M cfg-cache=`), so
  the calibration runs once. Run later with the `function` command it does the same poses and ends
  with its own CLI `save` (the snapshot stays the first start's). The poses are 3° off the axes, as a hand holds them: iNav 9.1's scale solver
  (`common/maths.c`, Gauss elimination without pivoting) divides by the X component of the Z-axis
  samples, which is 0 for exact poses, and its result check (`isnan && isinf`) lets the NaN through:
  accgain 0 and the `SETTINGFAIL` arming blocker.
- **The IMU's accelerometer error** (`trainer.json`: `icm42688p` `accel_offset_mg` 20,
  `accel_scale_pct` 0.5). A bench property, like a real chip's error, not a workaround: the calibration
  measures it (acczero 41 = 20 mg at 2048 LSB/g, accgain 4075/4076 = 4096 / 1.005). iNav takes exactly
  accZero 0 and accGain 4096 as "never calibrated" (`sensors/acceleration.c
  accSetCalibrationValues`), so an ideal sensor would keep the `ACC` blocker after a correct
  calibration. Every other bench leaves both props unset (no error); the values are part of the
  snapshot key.
- **Sensors (phase 4a).** The bench wires the GPS module (a u-blox M10 `ublox` on uart3 + its
  QMC5883L `qmc5883l` on I2C1, next to the board's SPL06) and a 4S 4200 mAh `battery` on BAT; iNav's
  `status` then shows `BARO=SPL06, MAG=QMC5883`, GPS and `Voltage: 16.79V (4S battery - OK)`
  (`core/qemu/evidence/sensors/README.md`). The battery is drained by `current_scale` × the ESC
  model's current + 0.3 A for the electronics and feeds nothing back into the aircraft.
- **Compass calibration** (`calibrate_mag`, `auto_first_start`, after `calibrate_acc`): iNav will not
  arm with an uncalibrated compass (`COMPASS`) and does not use it for the heading; its calibration
  (`MSP_MAG_CALIBRATION`, 30 s by `mag_calibration_time`) takes each axis's gain from the largest
  reading it sees, so the core turns the model until every chip axis, both ways, points along the
  earth field (poses computed from the compass's field and mount), with yaw turns level and inverted
  for iNav's sphere fit, in 18 s; the first start then saves it with the rest. The snapshot key
  covers the compass's field and mount.
- **Environment.** `env_default` (`env/launch.json`) starts with the core and after `reset aircraft`;
  `-M env-scenario=<name>` or the `env` command picks another (`ground-takeoff`).

## Configuration (`inav.cli`): as the iNav Configurator would leave it
A user connecting iNav Configurator 9.1.1 to a fresh board gets the "Default values" dialog, picks
**Airplane with a Tail**, and has a configured plane. `inav.cli` writes that same result, so the
configurator connecting to the bench sees a plane configured through it (no dialog, no red "Mixer not
configured", outputs on). References are to inav-configurator 9.1.1 and iNav 9.1.0.

- **The two flags.**
  - `applied_defaults = 3`: the dialog shows on every connect while this is 0
    (`js/defaults_dialog.js:33-36`, called from `js/serial_backend.js:314`); the preset writes its own id,
    3 (`js/defaults_dialog_entries.js:390`). "Keep current settings (Not recommended)" writes only
    `applied_defaults = 1` and does not reboot (`:885-896`, `defaults_dialog.js:310-313`). Range 0..99, "internal
    (configurator) hint" (`fc/settings.yaml:1615`).
  - `model_preview_type = 14`: the CLI name of the mixer's `appliedMixerPreset` (`fc/settings.yaml:1250`,
    per mixer profile, -1 by default), which `MSP2_INAV_MIXER` reports (`fc/fc_msp.c:1606`). The Status tab
    logs the red "Mixer not configured. Use Mixer tab to set it up!" when it is -1 (`tabs/setup.js:318-320`;
    also `tabs/magnetometer.js:722`), and the Mixer tab then preselects nothing and asks to load a preset
    (`tabs/mixer.js:926-958`). The dialog's airplane preset applies mixer preset 14, "Airplane"
    (`mixerToApply`, `js/model.js:399-427`).
- **The preset's settings** (`defaults_dialog_entries.js:380-592`), written as `setSettings` does
  (`defaults_dialog.js:203-293`): most outside the profiles; the control-profile ones (the list in
  `js/fc.js getControlProfileParameters`) on control profiles 1..3; `throttle_idle` (a battery-profile
  setting) on battery profiles 1..3; `nav_fw_alt_control_response` is a PID-profile setting the configurator
  does not list, so it lands in the current profile only (1). Two keys are not settings in 9.1.0 and the
  configurator skips them ("Invalid setting", `MSPHelper.js:3496`; the firmware's name lookup is `strcmp`):
  `gyro_hardware_lpf`, `nav_fw_pos_z_FF` (the setting is `nav_fw_pos_z_ff`). The preset has no feature list.
- **The mixer** (what `MSP2_INAV_SET_MIXER` + the rules carry, `defaults_dialog.js:256-276`): `platform_type
  AIRPLANE`, `has_flaps ON` (the preset's `hasFlaps`: iNav then offers the FLAPERON mode, `flight/mixer.c:198`),
  `motorstop_on_low ON` (default), one motor at full throttle, and the preset's servo rules, all +100:
  roll to both ailerons, pitch to the elevator, yaw to the rudder. The preset numbers the servos 1 elevator,
  2/3 flaperons, 4 rudder; this bench is wired servo 1..4 = aileron_l, aileron_r, elevator, rudder (S3..S6,
  above), so the rules keep the bench's servo numbers. The Mixer tab labels the outputs from the rules, not
  the preset (`tabs/mixer.js` `renderServoOutputImage`), so it shows them right.
- **Servo directions** as the Outputs tab's reverse writes them, `servo <n> 1000 2000 1500 -100` for aileron_r,
  elevator, rudder (were -100 `smix` weights: the same pulses, output = weight × rate, `flight/servos.c:466,474`).
  Why these three: roll right is left aileron down, right up; iNav's positive pitch/yaw are nose down/left
  while the FDM's +µs is elevator trailing edge up (nose up) and rudder trailing edge right (checked on the
  stand, phase 3).
- **`feature PWM_OUTPUT_ENABLE`.** The Outputs tab's "Enable motor and servo output" is not a configurator-local
  switch: it is this firmware feature (`tabs/outputs.html:11`, bit 28, `js/fc.js:637`; the local safety switch
  is a different one, "I understand the risks, propellers are removed", `#motorsEnableTestMode`). Neither the
  target's `DEFAULT_FEATURES` nor the airplane preset sets it; without it iNav configures the output pads as
  GPIO driven low (`drivers/pwm_output.c:189-196`), i.e. no ESC or servo pulses on a real board. The machine
  reads the pulse width from the timer's CCR whatever the pin mode, which is why the bench flew without it.
- **Kept from before:** ARM/ANGLE/NAV LAUNCH on ch5/6/7 (launch by the switch, not `feature FW_LAUNCH`).
  `feature -GPS` is gone since phase 4a: the wired `ublox`'s lines (`feature GPS`, `serial 2 +1`, ...) follow
  the script; the compass and the battery need no line (below, "Sensors").
- **What the preset changes in flight** (iNav default → preset): `looptime` 500 → 1000 µs; gyro LPF 60 → 25 Hz;
  D-term LPF 110 → 10 Hz; dynamic notch q 120 → 250, min 50 → 30 Hz; motor protocol ONESHOT125 → STANDARD;
  `throttle_idle` 8 → 5 %; rates roll/pitch/yaw 200/200/200 → 180/90/30 °/s, expo 70 → 30 (yaw 20 → 30); fixed-wing
  PIFF pitch 5/7/0/50 → 15/5/5/80, roll 5/7/0/50 → 15/3/7/50, yaw 6/10/0/60 → 50/0/20/255; TPA 0 → 80; turn assist
  pitch gain 1.0 → 0.4; D boost 0.5..1.25 → 1..1 (off; the fixed-wing D term uses it too, `flight/pid.c:786,853`); `nav_fw_pos_xy_p` 75 → 55; altitude PID 30/5/10 → 22/6/2, response 40 → 45; `airmode_type`
  STICK_CENTER → STICK_CENTER_ONCE; `small_angle` 25 → 180°; `nav_fw_control_smoothness` 0 → 2; RTH landing
  ALWAYS → FS_ONLY, RTH altitude 10 → 50 m; WP radius 1 → 8 m, WP max safe distance 100 → 500 m;
  **launch: motor delay 500 → 100 ms, max altitude none → 50 m** (the launch ends at 50 m or on its 5 s timeout).
  Unchanged although listed: `nav_fw_launch_max_angle` 45, `nav_fw_bank_angle` 35, `max_angle_inclination_rll` 450,
  `ahrs_inertia_comp_method` ADAPTIVE, `dynamic_gyro_notch_enabled` ON.
  `core/qemu/evidence/plane/README.md` has the flights with them (plane-launch; the ground take-off, whose failsafe
  landing now ends in a `crash` event, the rudder on its end stop in the failsafe turn).

### Where each line comes from: the layers
`inav.cli` is layer `aircraft` of the script the core types at the first start: after the board's
`defaults.cli` (layer `board`, empty today), before the wired devices' lines (layer `device`: the
`ublox` adds `feature GPS`, `serial 2 +1`, `set gps_provider = UBLOX`, `set gps_ublox_nav_hz = 10`) and the
bench's own lines (layer `bench`: a `bench_cli` file the app writes next to the bench). `session-check`
lists every line with its source (`config_src: [aircraft:models/trainer/inav.cli:25] set nav_fw_bank_angle = 35`);
a setting a later layer sets again with another value is reported as a conflict, and the later line wins
(a bench line `set nav_fw_bank_angle = 40` over this file's 35: `config_conflict: set nav_fw_bank_angle: ...`).
Edit a setting where it belongs: the aircraft's behaviour here, one bench's variation in its `bench_cli`
(or `disabled_lines: ["device:4"]` to leave out a generated device line), then `config apply` types the
change into the running firmware. Several of this file's values are iNav's defaults (`nav_fw_bank_angle`
35, `nav_fw_launch_max_angle` 45, `motorstop_on_low`...): `diff against:"script"` lists them as
`only_script` (the firmware's `diff all` does not show a default). `docs/protocol.md` has the layers,
`core/qemu/evidence/config-layers/` the trainer's.

## Visual (`visual/trainer.tscn` + `visual/TrainerVisual.cs`)
A Godot placeholder, moved by the FDM pose: fuselage box, 1.88 m wing slab, tail slabs (horizontal and
vertical), a prop disc, a small FRD axis gizmo at the CG. The pivots `aileron_l`, `aileron_r`, `elevator`,
`rudder` sit on their hinge lines and rotate by `state.plane.surfaces_deg` (+ = aileron trailing edge down,
elevator TE up, rudder TE right; without `plane`: `actuators` × 20°); `prop` spins at `rpm`. `cg_height`
0.1 m, `view_size` 1.88 m (the app's cameras scale by it). app/README.md → Plane.

Lessons carried over from the heli bench: never copy servo reverse flags from a real model; the
model does not reproduce linkage geometry. Surface directions come from the wiring's `reversed`.

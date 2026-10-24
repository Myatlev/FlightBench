# Core ↔ App protocol (v0.1, implemented by the QEMU core — `core/qemu/hw/arm/nexus_xr_ctl.c.inc`)

The **core** (QEMU machine + devices + FDM, headless) is a server. The **app** (Godot .NET) and
any script are clients. One TCP connection per client, newline-delimited JSON messages in both
directions. Numbers are SI unless noted. Time is the core's **virtual time** in seconds (`t`),
never host time. The core never blocks on a client: state is broadcast at a fixed virtual-time
period; commands are applied at the next step.

Default port: **31250** (the core's other ports: MSP 31235 = the board's MSP UART, USART1 on the
nexus-xr and USART6 on the speedybee-f405-wing; HMP 31236, motion 31237, pilot 31238, gdb 31234).
The core listens on 127.0.0.1; `CTL_HOST=0.0.0.0 core/qemu/run.sh ...` opens it to other machines.
Any number of clients; a client that does not read loses `state`/`samples` (dropped above 64 KiB of
backlog), never slows the machine; above 4 MiB it is disconnected.

## Framing
```
{"type": "...", ...}\n
```
Unknown fields are ignored; unknown `type` is answered with `{"type":"error","what":"unknown type"}`.
Any rejected command (bad JSON, out-of-range value, unreadable firmware file) is answered with
`{"type":"error","what":"..."}` to that client, and nothing of it is applied. `{"type":"ping"}` →
`{"type":"pong","t":..}`.

Numbers: every time `t` (and `timing.host_t`) is written with 4 decimals (0.1 ms at any run length);
an integer value is written without a fraction, any other number with up to 9 significant digits;
a value that is not finite (e.g. an attitude the firmware has not reported yet) is `null`.

## Core → client

### `hello` (once, on connect)
```json
{"type":"hello","core":"flightbench-qemu","version":"0.1","board":"nexus-xr","firmware":"rotorflight 4.6.0 STM32F7X2_OSD",
 "aircraft":"rs7","actuators":["swash_a","swash_b","swash_c","tail","motor"],"state_hz":100,
 "t":53.21,"fdm":true,"power":true,"paused":false,"osd":null}
```
`firmware` comes from MSP (FC_VARIANT/FC_VERSION/BOARD_INFO: `rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)`)
once the firmware has answered, before that it is the file name. `actuators` are servo1..4 and motor1
in that order (named by the session's wiring). `hello` is followed by `manifest` and `signals`, then
what happened before this client connected and is still true, each marked `"replay":true`: the
last `config` event (see `event`), the last `function` message and the last
`diff` (when there are any); the last `modes` comes on `subscribe` `modes:true` instead. The replayed `config` event and `diff` carry the current `t` (the diff's
`at` says when it was captured); the replayed `modes` and `function` keep the `t` they had.

`osd` = the OSD this machine models (also in every `state`), from the wiring, not the firmware:
- `{"kind":"msp_displayport","cols":50,"rows":18,"video":true,"uart":4}` — an `msp_osd` device (an HD
  VTX speaking MSP DisplayPort) is wired to that UART; the core streams what the firmware draws as
  `osd` messages (below): `video:true` = there is a picture to show. On the SpeedyBee F405 WING
  (iNav) this wins over the board's MAX7456: iNav picks the MSP OSD first and never initialises
  the chip.
- `{"kind":"max7456","cols":30,"rows":16,"video_system":"PAL","video":false}` — the board's analog
  OSD chip (the SpeedyBee F405 WING): modelled so the firmware's driver runs, but there is no
  camera and no rendering, so no picture (`video:false`, no `osd` messages). `rows` follow the
  chip's VM0 register as the firmware set it: PAL 16, NTSC 13; `video_system` `null` (and 16 rows,
  iNav's default) until the firmware wrote VM0.
- `null` — neither (the nexus-xr without an `msp_osd`).

### `manifest` (once, right after `hello`)
What this core can emulate — the core declares it, the app builds its wiring wizard, device
palette and fault menu from it. The same document is printed by `core/qemu/run.sh --manifest`
(and exits without starting the emulator), so the app can read it before any session exists.
```json
{"type":"manifest","core":"flightbench-qemu","version":"0.1",
 "boards":[{"id":"nexus-xr","name":"RadioMaster Nexus-XR","mcu":"STM32F722",
   "machine":"nexus-xr","config":"unified","msp_uart":"uart1","firmware_family":"rotorflight",
   "pads":[{"id":"gyro1","label":null,"kind":"spi","flags":[],"pins":{"cs":"A04","int":"B08"},"bus":"spi1",
            "onboard":true,"device":"icm42688p","detachable":true,"part":"ICM-42688-P","devices":["icm42688p"]},
           {"id":"uart4","label":"PORT-A","kind":"uart","flags":["5v"],"pins":{"tx":"A01","rx":"A00"},"serial":3,"onboard":false,"devices":["crsf"]},
           {"id":"uart2","label":"TELEM","kind":"uart","flags":["rx_only"],"pins":{"rx":"A03"},"serial":1,"onboard":false},
           {"id":"motor1","label":null,"kind":"motor","flags":[],"pins":{"signal":"A09"},"timer":"TIM1_CH2","dma":"DMA2_S6_CH0","onboard":false,"devices":["esc"]},
           {"id":"freq1","label":"RPM","kind":"capture","flags":[],"pins":{"in":"A02"},"timer":"TIM5_CH3","dma":"DMA1_S0_CH6","onboard":false}, ...],
   "slots":[{"id":"gyro1","kind":"spi","devices":["icm42688p"]},
   {"id":"i2c3","kind":"i2c"},{"id":"uart1","kind":"uart"}, ... ,{"id":"uart5","kind":"uart","devices":["crsf"]},
   {"id":"motor1","kind":"motor","devices":["esc"]},{"id":"servo1","kind":"servo","devices":["servo"]}, ...]},
  {"id":"speedybee-f405-wing","name":"SpeedyBee F405 WING","mcu":"STM32F405",
   "machine":"speedybee-f405-wing","config":"cli","msp_uart":"uart6","firmware_family":"inav",
   "slots":[{"id":"gyro1","kind":"spi","devices":["icm42688p"]},{"id":"i2c1","kind":"i2c"},{"id":"uart1","kind":"uart","devices":["crsf"]}, ...],
   "pads":[{"id":"output1","label":"S1","kind":"output","flags":[],"pins":{"signal":"B07"},"timer":"TIM4_CH2","onboard":false,"devices":["esc","servo"]}, ...]}],
 "devices":[
  {"id":"icm42688p","label":"ICM-42688-P gyro","category":"imu","kind":"spi",
   "props":[{"id":"gyro_noise_dps","label":"gyro noise RMS deg/s","type":"float","min":0,"max":50,"default":0.05},
            {"id":"accel_noise_g","label":"accel noise RMS g","type":"float","min":0,"max":2,"default":0.002}],
   "faults":[{"id":"freeze","label":"IMU output frozen"}],
   "signals":["imu.gyro_x_dps", ...],"implemented":true},
  {"id":"dps310","label":"DPS310 baro","category":"baro","kind":"i2c","props":[],
   "faults":[{"id":"freeze","label":"baro output frozen (result registers stop updating)"}],
   "signals":["baro.pressure_pa","baro.temp_c"],"ports":[{"id":"i2c","kind":"i2c","dir":"both","label":"I2C"}],"implemented":true},
  {"id":"crsf","label":"CRSF receiver","category":"receiver","kind":"uart","props":[],
   "faults":[{"id":"link_lost","label":"radio link lost (receiver silent, LQ 0)"},
             {"id":"freeze","label":"receiver frozen (repeats the last frame)"},
             {"id":"lq","label":"link quality","params":[{"id":"lq","label":"link quality %","type":"int","min":0,"max":100},
               {"id":"rssi_dbm","label":"RSSI dBm","type":"int","min":-130,"max":0,"optional":true},
               {"id":"snr_db","label":"SNR dB","type":"int","min":-30,"max":30,"optional":true}]}],
   "signals":["rc.ch1_us", ... ,"link.up","link.rssi_dbm","link.lq","link.snr_db","crsf.telemetry_frames"],"implemented":true},
  {"id":"esc","label":"ESC","category":"esc","kind":"motor",
   "props":[{"id":"actuator","label":"aircraft actuator","type":"actuator"},
            {"id":"telemetry","label":"RPM telemetry","type":"enum","options":["bidir","serial"],"default":"bidir"},
            {"id":"signal_timeout_ms","label":"signal-loss timeout ms (0 = never)","type":"int","min":0,"max":2000,"default":100}],
   "faults":[{"id":"telemetry_lost","label":"RPM telemetry lost"}],
   "signals":["out.motor{n}_dshot","out.motor{n}_bidir","esc.throttle","esc.motor_rpm","esc.erpm","esc.current_a","esc.load"],
   "ports":[{"id":"signal","kind":"motor","dir":"in","label":"throttle signal (DShot)"},
            {"id":"tlm","kind":"uart","dir":"out","label":"serial telemetry","optional":true},
            {"id":"rpm","kind":"capture","dir":"out","label":"RPM (frequency) wire","optional":true}],"implemented":true},
  {"id":"msp_osd","label":"HD VTX OSD (MSP DisplayPort)","category":"osd","kind":"uart","props":[],
   "faults":[{"id":"link_lost","label":"OSD device silent (ignores the FC, sends nothing: no picture)"}],
   "signals":["osd.frames"],"ports":[{"id":"uart","kind":"uart","dir":"both","label":"MSP DisplayPort serial"}],
   "config_firmware":{"rotorflight":["serial {serial} 1 115200 57600 0 115200","set displayport_msp_serial = {serial}", ...],
                      "inav":["feature OSD","serial {serial} 33554432 115200 115200 0 115200","set osd_video_system = HDZERO", ...]},
   "implemented":true},
  {"id":"servo","label":"Servo","category":"servo","kind":"servo",
   "props":[{"id":"actuator","label":"aircraft actuator","type":"actuator"},{"id":"reversed","label":"linkage reversed","type":"bool","default":false}],
   "faults":[],"signals":["out.servo{n}_us"],"implemented":true},
  {"id":"spl06", ... ,"implemented":true},{"id":"ublox", ... ,"implemented":true},
  {"id":"gps", ... ,"implemented":false},{"id":"qmc5883l", ... ,"implemented":true},
  {"id":"compass", ... ,"implemented":false},{"id":"battery", ... ,"implemented":true}, ...],
 "aircraft":[{"id":"rs7","name":"Goosky RS7 Ultra","class":"700 electric helicopter","fdm":"heli",
   "actuators":["swash_a","swash_b","swash_c","tail","motor"],"implemented":true}]}
```
- `boards`: every board this core emulates, nexus-xr first. `machine` = the QEMU machine type a
  bench on it runs (`-M <machine>`), `config` = how the firmware is configured (`unified`: a
  RotorFlight unified config and the custom-defaults blob; `cli`: a CLI script the core sends over
  the MSP UART, see Session description), `msp_uart` = the pad of the MSP/CLI host socket (`ports.msp`),
  `firmware_family` = the firmware the board runs (`rotorflight`, `inav`; board.json): which of a
  device's `config_firmware` line sets applies.
- `boards[].pads`: the board's pads (scripts/wiring.py). Firmware id, `kind`
  (`spi|i2c|uart|motor|servo|capture|output`), `pins`, `timer`, `dma`, `bus`, `serial` (the CLI's serial
  index) come from the unified config (a `cli` board: all from `boards/<id>/board.json`); `label` (the
  name printed on the board, `null` when not confirmed), extra `flags` (`rx_only`/`tx_only` from the
  config, `5v`, `usb`) and the onboard devices (`onboard:true`, `device`, `detachable`, `part`) from
  `boards/<id>/board.json`. An `output` pad (iNav's auto-assigned outputs, S1..S11 on the SpeedyBee)
  takes a device port of kind `motor` or `servo`. `devices` = what the emulated machine models on
  that pad. A session names a pad by its id or its label.
- `boards[].slots`: the old view, kept unchanged for the current app: from the board's unified config, by the rule the app's wizard uses
  (`app/scripts/WiringGraph.cs` `BoardInfo.Slots`): `gyroN` (`resource GYRO_CS N`), `i2cN`
  (`I2C_SCL`), `uartN` (`SERIAL_TX`/`SERIAL_RX`), `motorN` (`MOTOR`), `servoN` (`SERVO`), in that
  order (a pad of another group, e.g. an `output`, is not a slot); `kind` is the slot kind
  (`spi|i2c|uart|motor|servo`) a device's `kind` must match.
  `devices` (when present) = what the emulated machine models behind that slot. A session in the
  old form (`slot`) may name anything else: it is reported as a warning and left out, and the
  receiver, ESC and servos stay (see Session description). Nexus-XR: 13 slots.
- `devices`: each device model announces itself (a C descriptor in `devices/`, see
  `devices/include/fb/device.h`). `ports` are its connections: `kind` = the pad kind it plugs into,
  `dir` seen from the device (`in`/`out`/`both`; a UART port `out` needs the pad's RX), `optional` = the
  device works without it, `bundled` = comes with the primary port's pad (never wired alone); the old
  `kind` = the first (primary) port's kind. `props` are the options a wiring entry can carry (types
  `int|float|bool|enum|actuator`; `actuator` = one of the aircraft's `actuators`). `faults` are
  what `fault` accepts for that device, with their `params`; a param without `default` and not
  `optional` is required when turning the fault on. `signals` are the signal ids the device
  feeds (`{n}` = the slot number). `config` (when present) = firmware CLI lines the core appends to
  the custom-defaults blob when the device is wired (the firmware must be told about it: a serial
  function, a driver setting); `{serial}` = the serial index of the UART pad it is wired to. `implemented:false` = a model or a plan that this machine has
  not wired in (it is listed so the app can show it greyed out; wiring it does nothing).
  The SpeedyBee F405 WING's sensors (iNav; phase 4a, `core/qemu/evidence/sensors/`):
  - `spl06` (baro, port `i2c`), the board's onboard baro on `i2c1` (0x76): prop `noise_scale` (float
    0..20, default 1: pressure noise × the datasheet-like precision; 0 = exact). iNav detects it
    (`baro_hardware` AUTO → `SPL06` in its diff).
  - `qmc5883l` (compass, port `i2c`): the GPS module's compass, wired by the user to `i2c1` (0x0D).
    Props `mount` (the chip's orientation in iNav's alignment names, default `CW270FLIP` = iNav's
    `align_mag` DEFAULT for an external compass: the core outputs the field in the chip's axes and
    the firmware aligns it), `declination_deg` / `inclination_deg` / `intensity_ut` (the earth field;
    defaults 6.52 / 59.45 / 49.67 = Yerevan, WMM2025; the field does not follow `env` `home`: set the
    props for another place), `noise_mg` (σ per axis, default 2). The field in the chip is the earth
    field turned into the body by the FDM attitude. iNav needs its compass calibration before it
    arms or uses it: the aircraft's `calibrate_mag` function.
  - `battery` (battery, port `power`): the pack on the board's `batt1` pad (label BAT: VBAT C00 →
    ADC1 IN10 through the 11:1 divider of iNav's default `vbat_scale` 1100, the current sensor
    19.5 mV/A on IN11 = the target's `current_meter_scale` 195). Props `cells` (int 1..12, default
    4), `capacity_mah` (4200), `start_soc` (0..1, 1), `resistance_mohm` (0: no sag), `current_scale`
    (1: the current = this × the ESC model's current + `fc_current_a`), `fc_current_a` (0.3: FC,
    receiver, GPS). Voltage = cells × a LiPo OCV(SoC) curve (4.20 … 3.30 V per cell). The battery
    feeds nothing back into the aircraft (the ESC keeps the aircraft's own supply voltage). The FC's
    sensors saturate at the ADC's 3.3 V: 36.3 V through the divider (above 8S full, or a
    `battery.voltage` fault above it, reads 36.3 V) and 169 A on the current sensor (the pack's own
    `mah_used` keeps the true current). The pack is never reset by the core (`reset aircraft`,
    `power`, an MCU reset keep its SoC and `mah_used`, as a real pack would): a fresh pack is a new
    core start.
  - `ublox` (gps, port `uart` both): props `generation` (`m10`|`m8`, default `m10`: which of iNav's
    two configuration paths it takes), `baud` (power-on baud, default 9600; iNav's autobaud moves it
    to the port's GPS baud), `rate_hz` (power-on navigation rate, default 10; the firmware's CFG-RATE
    overrides it), `fix_delay_s` (3D fix after power-on, default 20), `noise_m` (position noise σ,
    default 0.3), `sats` (default 16), `pdop` (default 1.2), `h_acc_m` (reported horizontal
    accuracy, default 1.0), `seed` (noise PRNG, default 1), `epoch_unix` (UTC at power-on, default
    1790640000 = 2026-09-29 00:00:00). `config`: `feature GPS`, `serial {serial} +1` (adds iNav's
    FUNCTION_GPS to that port), `set gps_provider = UBLOX`, `set gps_ublox_nav_hz = 10`. Signals
    `gps.lat_deg`, `gps.lon_deg`, `gps.alt_m`, `gps.fix`, `gps.num_sat`, `gps.ground_speed_mps`,
    `gps.course_deg` (what the receiver sent last, faults applied). Position: the FDM's NED around
    the `env` `home` on WGS-84 (default 40.1792 N 44.4991 E at the baro's field elevation). Wired to a
    USART pad `uart1`..`uart5` (the target's GPS port: `uart3`, serial 2); one GPS per session.
- `aircraft`: `models/*/aircraft.json`; `implemented` = the core has an FDM for it (on the nexus-xr;
  the speedybee-f405-wing runs none yet).
- `aircraft[].functions`: the aircraft's **environment functions** — what the core does to the model
  and the firmware on command, as the bench's hands would (`function`, below):
  ```json
  "functions":[{"name":"calibrate_acc","label":"Calibrate accelerometer (6 positions)","auto_first_start":true,
                "scenario":"env/acc-calibration.json","params":{}},
               {"name":"calibrate_gyro","label":"Calibrate gyro (hold level and still, reboot)","auto_first_start":false,
                "params":{"hold_s":2}},
               {"name":"launch","label":"Hand launch","auto_first_start":false,
                "params":{"speed_kmh":50,"duration_s":0.5,"pitch_deg":10}}]
  ```
  `name` is one of `calibrate_acc`, `calibrate_gyro`, `launch`, `calibrate_mag` (each at most once); `label` what
  the app shows; `auto_first_start` = part of the first start of a CLI-configured board (after
  the config script, below; never `launch`); `scenario` (calibrate_acc only) the poses, a scenario
  file next to the aircraft; `params` the defaults a `function` command may override
  (`calibrate_gyro`: `hold_s` 0..60; `launch`: `speed_kmh` (0, 360], `duration_s` (0, 10],
  `pitch_deg` −60..60; `calibrate_acc`, `calibrate_mag`: none). The resolver checks all of it (an unknown name, key or
  param, a wrong type or range, a missing scenario file is an error naming `functions[i]...`). An
  aircraft without `functions` has none, except that the older `acc_calibration` key (a scenario
  path) still means a calibrate_acc with `auto_first_start`. The RS7 lists `calibrate_acc` (level)
  and `calibrate_gyro`, none automatic (its profile carries `acc_calibration`); the trainer all four,
  `calibrate_acc` and `calibrate_mag` automatic (`calibrate_mag` is part of the first start only when a
  `qmc5883l` is wired).

### `state` (periodic, `state_hz` in virtual time; default 100 Hz)
```json
{"type":"state","t":12.345,
 "pos_ned":[0.0,0.0,-3.2],"vel_ned":[0.1,0.0,-0.5],"quat":[1,0,0,0],"euler_deg":[0.5,-4.8,120.3],
 "gyro_dps":[0.2,-0.1,0.0],"accel_g":[0.0,0.0,1.0],
 "rpm":1800,"motor_rpm":16062,"actuators":{"swash_a":0.32,"swash_b":0.31,"swash_c":0.30,"tail":-0.05,"motor":0.62},
 "pwm_us":[1660,1655,1650,1475,1620],"no_signal":[],"dshot":1287,
 "on_ground":false,"osd":null,
 "fc":{"msp":true,"armed":true,"mode":"ANGLE","boxes":[0,1],"arming_flags":[],"cpu_pct":6,"cli":false,"power":true,
       "attitude_deg":[0.5,-4.8,120],"baro_alt_m":3.21,"rc_us":[1500,1500,1500,1606,2000,2000,1500,1500,1500,1500,1500,1500,1500,1500,1500,1500],
       "link":{"up":true,"rssi_dbm":-60,"lq":100,"snr_db":10}},
 "faults":[],
 "rc_us":[1500,1500,1606,1500,2000,2000,1500,1500,1500,1500,1500,1500,1500,1500,1500,1500],
 "timing":{"realtime_factor":0.39,"host_t":17.9,"realtime":false,"speed":1,"idle_skip":false,"idle_skip_pct":0}}
```
`actuators` are normalised −1..1 by name (the visual scene animates by these): servos
(µs − 1500)/500, negated for a wiring entry with `reversed`; the motor is the ESC throttle 0..1.
`pwm_us` is the raw firmware output for the wiring/debug panel: servo1..4 in µs, then motor1 as the
µs equivalent of its DShot throttle (1000 + 1000·throttle). An output with nothing wired to it (a
servo pad without a servo, the motor without an ESC) is `null` in both. A wired output without a
signal is `null` in `pwm_us` and named in `no_signal` (actuator names, `[]` normally): a servo whose pad
had no pulse at the last physics tick (1 ms) — the pad not in the timer's alternate function (iNav
without `feature PWM_OUTPUT_ENABLE`), the channel off, the MCU in reset or off, the firmware not yet
driving it after a boot; its `actuators` value stays the last position, as a servo without signal
holds — and the motor while the ESC is past its signal-loss timeout (its throttle 0 in `actuators`);
`dshot` is the raw DShot value (0..2047,
1..47 = commands). `pos/vel/quat/euler` are the FDM's (NED, body FRD); `gyro_dps`/`accel_g` are what
the core feeds the IMU, in the **chip frame**, before sensor noise (the noisy values are signals).
`rpm` is head speed, `motor_rpm` the ESC's. `rc_us` are the 16 transmitter channels the receiver
sends. `fc` is what the core itself polls over MSP on USART1 (~10 Hz each of MSP_STATUS,
MSP_ATTITUDE and MSP_RC, asked as MSP v2 `$X<` so its replies are told apart from a host tool's v1
traffic on the same UART; `msp:false` until the firmware answered, and while in CLI mode or powered off the
last values stay): `mode` = the active flight-mode boxes except ARM joined by `+` (`ACRO` when none),
`boxes` = the same active boxes (ARM included) as the **permanent ids** of the `modes` message
(`boxes[].id`): the firmware's status carries one bit per box *index* (RotorFlight MSP_STATUS: 32
bits; iNav MSP2_INAV_STATUS: its whole box bitmask), the core maps index → id with MSP_BOXIDS; absent
until the first `modes` read (a subscriber's or a request's), `[]` when none is active. The app ties a switch position (`modes`
ranges) to the box that turns on by this id — ids are stable across firmware versions and
configurations, indices are not. `osd` as in `hello`.
`cpu_pct` = average CPU load, `fc.rc_us` = MSP_RC, the channels the firmware decoded (after its
`map`: roll, pitch, yaw, collective, throttle, aux...; a frozen receiver shows here while `rc_us`
moves). `baro_alt_m` = MSP_ALTITUDE, the firmware's altitude estimate in m (on the Nexus-XR the
baro only: pressure altitude minus the ground offset it tracks while disarmed and holds while armed;
polled ~10 Hz only while the firmware reports the baro in MSP_STATUS, absent otherwise: no baro
wired, or wired but not detected). `link` is what the receiver model reports (injected faults applied: `link_lost` → up false,
LQ 0; `lq` → the fault's values). `faults` = the active injected faults as `device.kind`
(`fault`, below), empty when none. `realtime_factor` = virtual
÷ host time over the last ~1 s of virtual time.

**Plane FDM** (aircraft `fdm: "plane"`, the speedybee-f405-wing; `core/qemu/hw/arm/fb_fdm.c.inc`): the
same fields from the plane (`pos_ned`/`vel_ned`/`quat`/`euler_deg` the FDM's, `rpm` = the prop's rpm,
`on_ground` the FWD_ONLY ground), `actuators` by name as above (aileron_l, aileron_r, elevator,
rudder: −1..1 = (µs − 1500)/500 of the servo output; motor = ESC throttle), and one more object:
```json
"plane":{"airspeed_mps":13.9,"alpha_deg":2.1,"beta_deg":0.0,"thrust_n":21.4,
         "surfaces_deg":{"aileron_l":1.2,"aileron_r":-1.2,"elevator":-3.4,"rudder":0.0},
         "launch":"none|align|ramp","held":false,"posed":false,"scenario":"launch","wind_ned":[0,0,0]}
```
**Heli FDM** (aircraft `fdm: "heli"`, the Nexus-XR benches): with the FDM on the state carries the rotor's own
numbers, `"heli":{"coll_deg":6.21,"thrust_n":31.4,"torque_nm":3.9,"tail_pitch_deg":4.8,"wind_ned":[0,0,0]}` — the
blade collective pitch the model flies with (degrees, read back from the swash servos through the plate's geometry;
the signal `fdm.coll_deg`), the rotor thrust (`fdm.thrust_n`), the rotor's shaft torque, the tail rotor's blade pitch
and the wind set with `env` (docs/fdm.md §2). The app's Aircraft window shows the first two in the rotor row. Absent
without an FDM (a bare board bench).
**The F405's sensors** (the trainer bench): `fc.baro_alt_m` = iNav's altitude estimate (baro, and
GPS once it has a fix), polled while iNav reports the baro; `fc.gps`
`{"fix":2,"sats":16,"lat":40.1792003,"lon":44.4990998,"alt_m":0,"speed_mps":0.02,"course_deg":0}` =
`MSP_RAW_GPS` (fix = iNav's `gpsFixType_e`: 0 none, 1 2D, 2 3D; iNav's own solution; ~10 Hz, polled only while iNav reports
the GPS in its status); a wired battery adds
`"battery":{"voltage_v":16.8,"current_a":0.3,"mah_used":6.7,"soc":0.9984,"cells":4}` (the pack's
truth: what the FC's sensors see is this unless a `battery` fault is on).
`surfaces_deg` are the FDM's surface positions after the servo delay and slew, in degrees of
`servo_deflection_deg` (+ = aileron trailing edge down, elevator trailing edge up, rudder trailing
edge right; the wiring's `reversed` already applied); `launch` the launch constraint's phase, `held` a
hold (a `constraint`, a scenario's `hold`, the launch's alignment), `posed` held still at its pose,
`scenario` the running `env.scenario` (null when none or ended), `wind_ned` the `env` wind. Signals
`fdm.airspeed_mps`, `fdm.alpha_deg`, `fdm.beta_deg` (NaN/null with the heli). `idle_skip` = the session's `time.idle_skip`;
`idle_skip_pct` = share of virtual time over the same window that the core fast-forwarded instead of
executing the firmware's idle polling (0 when off).

### `osd` (MSP DisplayPort OSD: one message per frame the firmware draws)
Sent when the session wires an `msp_osd` device — the OSD side of an HD video transmitter
(HDZero / DJI / Walksnail class) speaking MSP DisplayPort — to a free UART (nexus-xr: PORT-A/B/C =
uart4/uart6/uart3; speedybee-f405-wing: uart1..uart5, the trainer's OSD bench uart4); without it
the core sends none. What the firmware draws is what the app shows:
```json
{"type":"osd","t":15.0917,"frame":161,"rows":18,"cols":50,"clear":false,"link":true,"font":"rotorflight",
 "cells":[{"r":1,"c":25,"s":"00:13","attr":0},{"r":15,"c":1,"s":"180","attr":0}]}
```
- **Rate**: one message per DRAW_SCREEN the device receives that changed a cell (a full frame is
  always sent): RotorFlight draws at `osd_framerate_hz` (default 12; 83 ms apart; ~11.5 messages/s
  armed on the RS7, the rest changed nothing). iNav 9.1.0 sends a DRAW whenever it wrote a cell, up to 62.5 Hz (its OSD task at 250 Hz,
  every 4th call): with the horizon on, ~47 DRAWs/s on the ground, most of them rewriting the same
  glyphs (the horizon is erased and drawn again each refresh); those are not sent, so the rate
  follows what moves. `frame` = the device's DRAW count (signal `osd.frames`); it skips the DRAWs
  that changed nothing.
- **`font`** (`"rotorflight"` | `"inav"`; absent while the firmware is unknown): which OSD font the
  codes index. On the speedybee-f405-wing the device asks the firmware MSP_FC_VARIANT (as a VTX does:
  `INAV` → `inav`, `RTFL` → `rotorflight`); until its first answer, and on the nexus-xr (no poll,
  below), the control server's own FC_VARIANT is used. The fonts
  differ (below); the app picks its atlas by this field.
- **Canvas**: `rows` × `cols` = the device's, 18 × 50 (HDZero's HD grid). The device announces it
  to the firmware (MSP_SET_OSD_CANVAS, below) and RotorFlight then draws for 50 × 18.
- **Delta / full**: `clear:false` = the cells that changed since the previous message (blanks
  included, e.g. `DISARMED` disappearing is 8 blank cells). `clear:true` = a full frame: wipe the
  canvas (blank = 0x20, attr 0), then write `cells` (every non-blank cell). A client gets a full
  frame first — at once on connect and on `subscribe` `osd:true` when the device has shown a frame,
  else as its next message — and every client gets one when the device sees a new contact: the
  first frame after an MCU reset (`reset`, `power`, a CLI `save`/`exit`, the firmware's own
  reboot) or after silence on the wire (below), after a DisplayPort RELEASE, and after the
  `msp_osd.link_lost` fault is turned off. `osd` messages are never dropped for a slow client (a
  lost delta would corrupt its canvas); the 4 MiB limit still applies.
- **`link`** (bool, default `true`; the core always sends it): `false` = the picture is gone and
  the app greys the panel. Sent once per loss as `{"type":"osd",...,"clear":true,"link":false,
  "cells":[]}` (wipe, nothing to draw; `frame` = the last DRAW count) when: `power` off, any MCU
  reset (`reset`, CLI `exit`/`save`, the firmware's reboot; right after the `reboot` event),
  `msp_osd.link_lost` on, the firmware entering CLI mode (RotorFlight's DisplayPort sends nothing
  while `cliMode`; the core knows it from `Entering CLI Mode`, `fc.cli`), or silence on the wire
  longer than the device's threshold = max(0.25 s, 3 × the frame interval) (frame interval = the
  longest of the last 8 DRAW gaps ≥ 16 ms, 1 s until measured: at the default 12 Hz 0.25 s, at
  `osd_framerate_hz = 2` 1.5 s). The next real frame is full (`clear:true`) with `link:true`. A
  client that connects or turns `osd` on while the link is down gets the `link:false` message
  instead of a stale canvas.
- **`cells`**: runs of consecutive cells of one row with the same `attr`; `r` 0..rows−1, `c`
  0..cols−1. `s` has one character per cell: the firmware's byte as the code point of the same
  value (U+0000..U+00FF, i.e. Latin-1; printable ASCII as is, the rest JSON-escaped `\u00NN`) = the
  glyph index in the OSD font, together with the page in `attr` for iNav:
  - `font:"rotorflight"`: the Betaflight/RotorFlight layout, 256 glyphs, glyph = the byte (letters
    and digits at their ASCII codes, symbols elsewhere; seen here: 0x9B/0x9C the timer symbols,
    0x72 0x73 0x74 the crosshair, 0x96 a battery, 0x06 the volt sign, 0xA0..0xFF the boot logo).
  - `font:"inav"`: iNav's own font, 512 glyphs (`drivers/osd_symbols.h`; the configurator's
    `resources/osd/analogue/default.mcm` has all 512, `resources/osd/digital/*` the HD PNGs by
    code), glyph = `(attr & 3) << 8 | byte`. Upper-case letters, digits and most punctuation at
    their ASCII codes; 0x60..0x7E are symbols, not lower case (0x76 SYM_ALT_M, 0x63 SYM_BATT_FULL,
    0x6A SYM_AMP), as are `"` `$` `'` `?` (0x22 0x24 0x27 0x3F); page 1 (0x100..0x1FF) holds the
    logo, the horizon (0x14C..0x154), its sidebars (0x131) and the crosshairs (0x13A 0x166 0x13B).
    The same code is a different glyph in RotorFlight's font: iNav's OSD needs iNav's atlas.
- **`attr`**: the attribute byte of MSP_DP_WRITE_STRING, passed through unchanged. iNav 9.1.0
  (`io/displayport_msp_osd.c` `drawScreen`, `displayport_msp_osd.h`) uses the same bits with a 2-bit
  page (`DISPLAYPORT_MSP_ATTR_FONTPAGE_MASK` 0x3) = bits 8..9 of its character code, so its cells
  carry 0x00 or 0x01 (page 0/1 of the 512-glyph font); it never sets blink (its MSP OSD reports no
  text attributes, so `drivers/display.c` blinks an element in software by blanking it every other
  phase) nor the version bit. RotorFlight 4.6 (`src/main/io/displayport_msp.c` `writeString`,
  `io/displayport_msp.h`):

  | bits | mask | meaning |
  |---|---|---|
  | 0..5 | 0x3F | font page = `displayport_msp_attrs[severity]` (CLI; 4 values for the element severity NONE / INFO / WARNING / CRITICAL; `0,0,0,0` by default, so 0 unless configured) |
  | 6 | 0x40 | blink (`DISPLAYPORT_MSP_ATTR_BLINK`): the element is in its blink state (e.g. the battery voltage below its alarm). RotorFlight sets it whether or not `displayport_msp_use_device_blink` is on; with it OFF (default) the firmware also blinks the element itself by leaving it out of every other 250 ms phase, so the cells come and go at 2 Hz and carry 0x40 while shown |
  | 7 | 0x80 | format/version flag (`DISPLAYPORT_MSP_ATTR_VERSION`), always 0 (DisplayPort v1/v2) |

  There is no invert/highlight bit. For a blinking element RotorFlight indexes
  `displayport_msp_attrs` with its internal blink flag (0x80) still set — `attrValues[0x80 + severity]`,
  past the 4-entry array — so bits 0..5 of a blinking cell are whatever byte lies there (0 on
  this build: the voltage cells arrive as 0x40).
- **The wire** (what the device parses; both firmwares push MSP v1 `$M>` frames of MSP_DISPLAYPORT
  182, RotorFlight on the port `displayport_msp_serial` names, iNav on the port with
  FUNCTION_MSP_OSD): sub-command 0 HEARTBEAT, 1 RELEASE (canvas blanked, next frame full),
  2 CLEAR_SCREEN (RotorFlight: every OSD frame starts with it; iNav: once per
  `osd_msp_displayport_fullframe_interval`, 1 s, followed by every non-blank cell, and when the OSD
  redraws the screen), 3 WRITE_STRING `[row][col][attr][chars]` (RotorFlight ≤ 30 chars, the
  whole element; iNav only the changed cells, runs of one row and one page, ≤ 50; clipped at the
  canvas edge, counted), 4 DRAW_SCREEN (= one frame: the drawing buffer becomes the shown canvas;
  writes without a CLEAR accumulate, which is how iNav's deltas work), 5 OPTIONS (RotorFlight: unused;
  iNav: `[font 0][mode]` with every CLEAR from its `clearScreen`, mode 0 SD 30 × 16, 1 HD 50 × 18,
  2 60 × 22, 3 53 × 20 — counted and kept, the bench line's `osd_mode`), 6 SYS `[row][col][element]`
  (a goggle/VTX-rendered element such as goggle voltage or bitrate: counted, not drawn — nothing
  here knows those values; iNav does not send it). Toward the firmware the device sends `$M<`
  requests, as an HD VTX does: MSP_SET_OSD_CANVAS 188 `[cols=50][rows=18]` once per contact
  (RotorFlight takes it, below; iNav has no such command and answers `$M!`, counted), and
  MSP_FC_VARIANT 2 every 125 ms from power-on (one at a time; none while the power is off or
  `link_lost` is on; on by default on the speedybee-f405-wing, off on the nexus-xr, `-M osd-poll=on|off`).
  iNav needs the latter: it pushes nothing until the VTX has sent it an MSP
  request, and stops 1 s after the last one (`io/displayport_msp_osd.c` `output()` needs
  `vtxActive`, set by `processMspCommand`, cleared by `checkVtxPresent` after `VTX_TIMEOUT` 1000 ms;
  "VTX sends an MSP command every 125ms or so"). RotorFlight pushes regardless; it answers every
  request while its MSP task runs (not during boot, the canvas save + reboot, or CLI mode), and the
  requests move its scheduling (the RS7-osd flight's FDM log differs with them), so the RotorFlight
  board leaves the poll off and its benches stay as they were.
- **Canvas mechanism** (RotorFlight 4.6, `msp/msp.c` MSP_SET_OSD_CANVAS, `io/displayport_msp.c`
  `redraw`): the firmware stores the announced size in `osdConfig` (`canvas_cols/rows`, default
  53 × 20; no CLI setting exists) and uses it when the display is (re)initialised. If the OSD was
  not yet `vcd_video_system = HD` with `osd_displayport_device = MSP`, it sets both, saves and
  reboots — on a fresh flash that is one extra `reboot {"cause":"reset"}` ~0.2 s after the first
  contact (the device's CLI lines set `osd_displayport_device = AUTO`, after the session profiles,
  so the firmware takes this path and keeps 50 × 18 in flash even when a profile set MSP + HD;
  with both already set it would keep the size in RAM only); from then on it draws 50 × 18 from boot
  (MSP_OSD_CANVAS 189 reads back 50, 18). Without the announcement this build's HD default is 53 × 20.
  iNav 9.1.0 has no announcement: its canvas is `osd_video_system` (`HDZERO` = 50 × 18, the
  device's iNav lines set it; `AUTO`/`PAL` would draw 30 × 16 into the 50 × 18 canvas's corner).

### `log` (firmware/core text lines: CLI output, core diagnostics)
```json
{"type":"log","src":"core|fc|cli","t":12.3,"text":"..."}
```

### `event`
```json
{"type":"event","t":12.3,"name":"armed|disarmed|crash|failsafe|reboot|beep|motor_beep|firmware_loaded|fault|launch|pose|scenario_end|mix_warning|config|cli","data":{}}
```
Plane FDM events (phase 3 wiring): `launch` `{phase:"start"|"release", ...}` and `pose` (`launch`,
`constraint`), a scenario step's own event name with `{label, index, step, scenario}` and
`scenario_end` (`env.scenario`), `mix_warning` `{left, right}` when both ailerons are deflected the
same way for 0.2 s (a servo mix or `reversed` mistake; the FDM uses (L − R)/2, so that pair gives no
roll). A plane's `crash` is |roll| > 30°, pitch < −20° or sink > 2.5 m/s at the contact.
`beep`: the beeper pin went active/inactive (`data.on`); `motor_beep`: a DShot beacon command
(`data.cmd` 1..5) reached the ESC. The app turns both into sounds.
The pin comes from the board config's `resource BEEPER 1 <pin>` (+ `beeper_inversion`); the Nexus-XR
has no beeper resource, so on this board the firmware beeps only through the ESC (`beacon ...` CLI
flags; e.g. the BEEPER mode switch → `motor_beep` every ~0.3 s). One `motor_beep` per beacon burst
(the command repeated in consecutive frames counts once); other DShot commands (6..47) arrive as
`dshot_command`. Data of the others: `reboot` `{"cause":"power-on|reset|power-off"}` (every MCU reset,
including the firmware's own after it wrote its config), `failsafe` `{"on":bool,"rxloss":bool}`
(RotorFlight: its FAILSAFE/RXLOSS arming flags; iNav: the FAILSAFE mode box, or its
FAILSAFE_SYSTEM/RC_LINK arming flags, which iNav updates only while disarmed), `armed`/`disarmed` (from MSP_STATUS; `disarmed`
`{"reason":"reset"}` when a reset ends an armed state), `power` `{"on":bool}`, `reset`
`{"what":"aircraft","firmware":bool}`, `firmware_loaded`, `cli` `{"open":bool,"by":"core"|"client",
"via":"control"|"msp"}` (the firmware entered its CLI — its banner seen — or left it at the next MCU
reset; `by:"core"` = the core's own sessions: the config script, `diff`, a function's save;
`"client"` via `control` = a client's `cli` command, via `msp` = a host tool's `#` on the MSP port,
e.g. the configurator's CLI tab; `state.fc.cli` follows it), `config` (boards configured by a
CLI script, `config: cli`) `{"state":"applying|calibrating|applied|restored|failed","lines":N}` — the
**first start** on a blank config sector: `applying` when the core enters the CLI with the script
(`applying` also carries `"sent":[{"text","layer","source"}, ...]`: the lines this run types, each
with its layer and source as in the report's `config_json`; `lines` stays their count);
then, when the aircraft has functions with `auto_first_start` (the trainer: `calibrate_acc`), the
script's `save` (the firmware reboots), `calibrating` `{"function":"calibrate_acc"}` and those
functions one after the other (`function` messages, below; the trainer's: iNav's 6-position
accelerometer calibration, the scenario's poses, `MSP_ACC_CALIBRATION` at each,
`MSP_CALIBRATION_DATA` until the firmware has the position; an `env` scenario or `reset aircraft`
meanwhile takes the model away, and the poses start over), then the CLI again; before the final
`save` the core runs `diff all` in the same CLI session (the `diff` message, `source:"first_start"`:
the configuration being saved, no extra reboot); `applied` after that `save` wrote the config sector
(kept as the snapshot for this firmware + script + the automatic functions [their names and
scenarios, and the IMU's static error they measure], with its diff, in memory and in `-M
cfg-cache=<dir>`, default `<user cache dir>/flightbench/config`, on Windows
`%LOCALAPPDATA%\flightbench\config`: `<machine>-fw-<hash>-script-<hash>.bin` + `.diff.txt`;
`"functions":["calibrate_acc"],"acc_calibrated":true` after them; a `config apply` run: `applying` /
`applied` with `"apply":true`, `"delta":true` when only the lines the sector lacked were typed,
`"removed":[...]`, `"restart_needed":true` when the bench's machine options changed, below),
`restored` when a blank sector got
that snapshot back instead (and its diff, `source:"snapshot"`), `failed` +
`"line":N,"text":"...","error":"..."` (the firmware's `### ERROR` line for a rejected line, or a
timeout / reset before the save; the firmware then runs its defaults) or `"error":"...",
"function":"calibrate_acc"` (a first-start function failed: the script's configuration stays saved
without it). **Replay:** the last `config` event goes again to every client that connects later,
right after `hello`/`manifest`/`signals`, with `"replay":true` in its data and the current `t` (a
snapshot restore happens at t = 0, before any client can connect).
**The bench state** (`-M bench-state=<file>`, launch contract below): the firmware's config belongs
to the bench. After every save of the firmware the core observes (the flash locked again after
writes into the config sector: CLI `save`, the configurator's `MSP_EEPROM_WRITE`, the calibrations'
own saves) it writes the sector to `<file>` and its metadata to `<file minus .bin>.json`, each
through a temporary file renamed over the old one. At start (a blank sector) the bench state wins
over the first-start snapshot when its metadata's `firmware` hash and `machine` match; the snapshot
(`cfg-cache`) stays untouched as the reproducible baseline. Then the script the bench assembles now
is compared with the metadata's `script` (the one the sector has, line by line, comments/`save`/`exit`
dropped): identical → `restored` `{"source":"bench"}` and nothing else; lines added or changed (e.g.
the GPS's lines after wiring a GPS) → `restored` `{"source":"bench"}`, `applying`
`{"delta":true,"lines":N}` with **only those lines** + `save`, `applied` `{"delta":true}`; lines the
script no longer has are listed in `"removed":[...]` of these events and **not reverted** (the
configurator may have set them since; the user resets or edits). Who wins for one setting: a
script line that is unchanged is never typed again, so what the configurator saved over it stays; a
script line that changed on the bench (the new line is in the delta) is typed after the restore and
wins over the configurator's value. The delta is typed in the script's order without the unchanged
lines around it: a delta line with side effects on other settings (e.g. a new `mixer_preset`, which
resets the mixes) is not followed by the unchanged lines it wiped — `config reset` for those. Then, once the firmware answers,
the automatic functions run only when their result is missing: the firmware reports the sensor
not calibrated (iNav's ACC/COMPASS arming flags; RotorFlight's ACC flag), the function never ran on
this bench, or its inputs changed (its scenario, the IMU's static error / the magnetometer's mount
and field it measures) → `calibrating` `{"function":..,"source":"bench"}`, then `applied`
`{"functions":[..],"source":"bench"}`. The file is ignored (logged, the first start or the snapshot
instead) when it is missing, not the sector's size, its metadata is unreadable or of another format,
machine or firmware; a `format` newer than the core's (a later core wrote it) is neither restored
nor overwritten — the bench state is off for that run (logged), so going back to an older app
does not destroy it; a sector whose `sector_sha256` differs from the metadata's (a crash between the
two renames) is used with a log line. The first start's own saves before the script's `save` are
not kept (the sector is not the bench's yet). A CLI board with **no script** (a bare firmware
on the bench: no device or aircraft lines) has no first start: every save of the firmware from the
boot on is kept (its first boot's defaults write included; metadata `script:""`), a blank sector at
start gets it back (`restored {"source":"bench","lines":0}`). A `{"type":"config","reset":...}` (below) discards it.
Metadata (`format` 1):
```json
{"format":1,"machine":"speedybee-f405-wing","firmware":"3bfe4bef168e...","sector_sha256":"b58288d8...",
 "script":"<the script text the sector has>","functions":{"calibrate_acc":"ba083378cf1d","calibrate_mag":"d73fa64c717d"},
 "saved":"2026-09-30T00:45:12+04","t":71.71,"saves":12,"diff":"<the last diff all text>|null","diff_stale":false}
```
`functions`: the automatic functions whose results the sector holds, each with the key of its
inputs. `diff` is the last `diff all` the core has (`stale` when the firmware saved since), sent as
the `diff` message (`source:"bench"`) on restore. `crash`
`{"roll_deg":-68.8,"pitch_deg":11.8,"speed_mps":4.40}`, `fault` `{"device":"crsf","kind":"link_lost","on":true}`
(every change of an injected fault, whichever message caused it — `fault`, `link`, the pilot socket) (`crash` once per hard ground contact: |roll| or
|pitch| > 15°, sink > 1 m/s or drift > 2 m/s at the contact; attitude and speed at that moment).
`crash` is informational: nothing is blocked. On every ground contact the FDM holds the model on the
ground (velocity zero) and rotates it level in 0.25 s, heading kept, with body rates the IMU reads
(on its side ≈ 360°/s), so the firmware's attitude follows; thrust still acts, the pilot decides what
to do next. The quaternion never jumps.

### `modes` (the firmware's modes: boxes and switch ranges, read over MSP, never written)
```json
{"type":"modes","t":1.6811,"variant":"RTFL",
 "boxes":[{"id":0,"index":0,"name":"ARM"},{"id":1,"index":1,"name":"ANGLE"},{"id":2,"index":2,"name":"HORIZON"},
          {"id":47,"index":3,"name":"TRAINER"},{"id":53,"index":4,"name":"RESCUE"}, ...],
 "ranges":[{"slot":0,"channel":0,"rc_channel":5,"rx_channel":4,"box":0,"start_us":1700,"end_us":2100,"logic":"or"}],
 "rx_map":[0,1,3,2,5,4,6,7]}
```
What the firmware's configuration says about modes, as the configurator shows it; the core never
writes modes (the configurator does), the app derives switch positions from this. Read by the core
over MSP (v2, on the MSP UART, between its periodic polls): `MSP_BOXNAMES` 116 (RotorFlight: pages of
32, the second page asked for when the first is full), `MSP_BOXIDS` 119, `MSP_RX_MAP` 64,
`MSP_MODE_RANGES` 34 and, on RotorFlight, `MSP_MODE_RANGES_EXTRA` 238 (iNav has none: it answers an
error). **Only on request or for subscribers:** with no client subscribed (`subscribe`
`modes:true`) the core never reads the modes by itself — the extra MSP requests shift the
firmware's timing, and the board must behave as without the core's reads (the regression flight
hashes). While a client is subscribed the core reads them after each boot once the firmware answers
(so after `config` applied / restored, after any CLI `save` or `exit`, a `firmware` load, `reset
firmware`, `power`) and ~0.3 s after the MSP host (a configurator) sent `MSP_SET_MODE_RANGE` 35 or
`MSP_EEPROM_WRITE` 250; subscribing sends the last read at once (`"replay":true`) when nothing
changed since, else reads now. A request (`{"type":"modes"}`) always reads. Each result goes to the
subscribers and to the clients that asked.
- `boxes`: every mode box the firmware offers, in its order: `index` = the box's position (the bit
  of the status bitmask, `state.fc.boxes` maps them), `id` = its **permanent id** (stable across
  versions and targets; iNav 9.1: ARM 0, ANGLE 1, NAV LAUNCH 36, FAILSAFE 27...; RotorFlight 4.6:
  ARM 0, ANGLE 1, RESCUE 53, GOVERNOR BYPASS 57...), `name` as the firmware names it.
- `ranges`: the activation slots in use (start < end, as the firmware tests them), `slot` = the
  configurator's row, `box` = the permanent id it turns on, `channel` = the aux channel index as
  the firmware stores it (0 = the first aux), `rc_channel` = the firmware's rc channel (0-based,
  `fc.rc_us` index: aux + 4 on iNav after roll/pitch/yaw/throttle, aux + 5 on RotorFlight after
  roll/pitch/yaw/collective/throttle), `rx_channel` = the receiver channel it reads (0-based, the
  `rc`/`rc_us` index the app sends: `rx_map[rc_channel]` for the mapped channels — iNav maps 4,
  RotorFlight 8 — the channel itself past them), `start_us`..`end_us` = active while start ≤ value
  < end (iNav: 900 + 25 × step; RotorFlight: 1500 + 5 × step). RotorFlight only: `logic`
  (`or`|`and` with the box's other slots) and `linked` (a slot linked to another box's state by its
  permanent id; `start_us`/`end_us` `null` then).
- `rx_map`: MSP_RX_MAP, the receiver channel each mapped firmware channel reads.
- Errors, to the client that asked: `{"type":"modes","t":..,"error":"cli"}` (the firmware is in
  its CLI: no MSP until it leaves it), `"power_off"`.

### `diff` (the firmware's `diff all`)
```json
{"type":"diff","t":40.71,"text":"# version\n# INAV/SPEEDYBEEF405WING 9.1.0 ...\nsave\n","stale":false,
 "source":"first_start","at":40.71}
```
The raw text of the firmware's CLI `diff all` (lines joined with `\n`, the command's echo left out,
up to its last line `save`). `source`: `first_start` (captured in the config script's CLI session
right before the final `save`: what was saved), `snapshot` (the snapshot's, restored with it),
`request` (a `diff` command, a `config snapshot`), `apply` (captured before a `config apply`'s save). `at` = when it was captured. `stale:true` = the firmware's config may
have changed since (a CLI `save` the core did not run — a host tool, the `cli` command, a function's
save —, the MSP host's `MSP_EEPROM_WRITE`, RotorFlight saving its ACC calibration; one `diff` message
with `stale:true` goes to every client when that happens; any firmware save other than the core's
script run marks it, before the bench state keeps the diff with the sector, so a restart restores it
as stale) or a refresh failed; `error` then says why. The text is the firmware's view of its configuration relative to its defaults — on the
nexus-xr the session's profile is RotorFlight's custom defaults, so only what differs from those
shows. The shipped starting config is a starting point: whatever the user sets later with the
configurator is what `diff all` shows. `source:"bench"`: the bench state's (restored with it; the metadata's `diff`).

### `function` (an environment function's progress)
```json
{"type":"function","t":14.79,"name":"calibrate_acc","phase":"step","step":1,"steps":6,"detail":"level (iNav position 0) collected"}
```
One message per phase of a function (`functions` in the manifest; `function` command below):
`phase` `start` (step 0; `"first_start":true` when the first start runs it), `step` (k of
`steps`), `done` (step = steps; extra fields: `"acc_calibrated":true`, `"gyro_calibrated":true`,
`"mag_calibrated":true`) or
`failed` (`detail` = why). A new client gets the last one (`"replay":true`). What each does:
- **`calibrate_acc`** with a poses scenario, iNav (the trainer): the scenario holds the model in
  each of iNav's six positions (`pose` events), at each the core sends `MSP_ACC_CALIBRATION` and polls
  `MSP_CALIBRATION_DATA` until the firmware has the position (steps 1..6; a position not collected
  within 2.5 s fails); then the CLI `save` (the firmware reboots; `done` after that reboot with the
  firmware's acczero/accgain). Run on command, it changes the board's config sector only: the
  first-start snapshot (the reproducible result of firmware + script + automatic functions) stays
  as it was, so the next session restoring it starts from the first start's calibration again.
  Run by the first start, the config flow saves it instead (with its
  `diff all`; `done` comes before that save). An `env` scenario or `reset aircraft` meanwhile starts
  the poses over; so does an MCU reset.
- **`calibrate_acc`** without a scenario, RotorFlight (the RS7): the firmware's level calibration
  as the model sits: `MSP_ACC_CALIBRATION`, the firmware reports `CALIB` while it samples, then
  saves the result itself (`acc_calibration`, no reboot); `done` once `CALIB` cleared and
  `NO_ACC_CAL` is not set. iNav without a scenario: `failed` (it only calibrates in six positions).
- **`calibrate_gyro`**: iNav and RotorFlight calibrate the gyro themselves at every boot and need
  the model still for it; there is no MSP command for it. The core holds the plane level and still
  (the heli sits on its skids), waits 1 s + `hold_s`, resets the MCU (flash kept: a `reboot`
  `{"cause":"reset"}`), and waits for the firmware's calibrating flag (iNav's `CAL` arming blocker,
  ARMING_DISABLED_SENSORS_CALIBRATING; RotorFlight's `CALIB`) to clear in its status: `done` with
  `"gyro_calibrated":true` and when it cleared, `failed` if still set 20 s after the reset. Then the
  plane goes back to the aircraft's default scenario.
- **`calibrate_mag`**: iNav's compass calibration (`sensors/compass.c`), needed before iNav arms
  with a compass or uses it (the `COMPASS` arming blocker: magzero 0/0/0 with maggain 1024/1024/1024
  counts as not calibrated). The core sends `MSP_MAG_CALIBRATION` (step 1 when answered): for
  `mag_calibration_time` (20..120 s, default 30) iNav fits a sphere to the field samples and takes,
  per chip axis, the largest reading as that axis's gain, so the heading is right only if every axis
  has seen the whole field. The core turns the model through a sweep it computes from the compass's
  field and mount: each chip axis, both ways, along the field (the shortest rotation onto it, held
  0.5 s), level and inverted yaw turns between them, in 18 s (step 2 at its end, the model held
  level); `done` once iNav's `COMPASS` blocker cleared (its result, magzero/maggain read with
  `MSP_CALIBRATION_DATA`, in `detail`; iNav saved it itself), `failed` if not within 130 s after the
  sweep, or when the sweep is replaced (`env`, `reset aircraft`) or the MCU resets. Needs the plane
  FDM and a wired `qmc5883l`; the first start runs it after `calibrate_acc` and saves it with the rest.
- **`launch`**: the `launch` command (below) with the function's `params` under the command's;
  `start` when applied, `step` when the ramp begins, `done` at the release (speed, height). The
  `launch` command and the pilot socket's launch line report the same messages.
Calibrations are refused (`error`) while the firmware is armed, in its CLI, powered off, while
another calibration or the first start runs, or while the core reads the diff.

### `prof` (host-time profile; only from a core started with `-M prof=on` / `PROF=1 run.sh`)
Where the **host's** time went since the core started (or the last `prof` reset): answer to the
`prof` command, and every `prof-period-s` virtual seconds to every client when that machine option is
set (`PROF_PERIOD=<s>`; default 0 = only on request). `core/qemu/scripts/prof.py` records, prints and
compares these (`core/qemu/evidence/prof/README.md`). One line, abridged (the RS7 liftoff to 45 s on the homelab):
```json
{"type":"prof","wall_ns":84480460684,"virt_ns":45000171837,
 "host":{"cpu":"Intel(R) Core(TM) i7-4790 CPU @ 3.60GHz","os":"Ubuntu 24.04.3 LTS (Linux 6.8.0-139-generic)","qemu":"11.1.1","build":"flightbench 1b3b615, 13.3.0"},
 "session":{"firmware":"rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)","icount_shift":2,"idle_skip":true,"idle_skip_mode":"budget","realtime":false,"speed":1,"fdm":true,"paused":true,"power":true,"prof_period_s":0},
 "accounts":[{"name":"nexus-xr.spi1","kind":"mmio","thread":"vcpu","count":1745067,"host_ns":424297988,"self_ns":424297988,"virt_ns":0},
             {"name":"fdm.tick","kind":"timer","thread":"vcpu","count":45000,"host_ns":197020113,"self_ns":17222433,"virt_ns":0,"deadlines":44979},
             {"name":"armv7m.nvic","kind":"mmio","thread":"vcpu","count":16653344,"host_ns":3028913473,"self_ns":3028913473,"virt_ns":0,"sampled":7},
             {"name":"ctl.rx","kind":"chardev","thread":"main","count":1681,"host_ns":83277539,"self_ns":16078580,"virt_ns":0,"bytes":28723}, ...],
 "handoffs":{"count":1387033,"by_reason":{"timer_deadline":334515,"idle_skip":921548,"timer_deadline_realtime":0,"timer_rearm":130970,"io":0,"halt":0,"other":0},
             "by_timer":{"crsf.frame":437314,"dma2.done":260382,"fdm.tick":44979,"not_machine":44998,...},
             "wake_ns":{"count":1157417,"mean":7502,"p50":4224,"p99":39936,"min":113,"max":6846121},
             "kick_ns":{"count":4,"mean":125887,"p50":35840,"p99":270336,"min":4467,"max":273618}},
 "mainloop":{"iters":1160525,"poll_ns":76785761019,"dispatch_ns":7572783111,"fds":11604839},
 "threads":{"vcpu":{"accounted_ns":21016198866,"unaccounted_ns":63464261818,"accounted_ratio":0.2488},"main":{"accounted_ns":84358544130,"unaccounted_ns":121916554,"accounted_ratio":0.9986}},
 "guest_exec_or_unaccounted_ns":63464261818,"accounted_ratio":0.2488,"ns_per_tick":0.27842}
```
- `wall_ns` host time, `virt_ns` virtual time, both since the start/reset. Host time comes from the
  CPU's tick counter (`cpu_get_host_ticks`, rdtsc on x86), calibrated against the monotonic clock
  once at start (`ns_per_tick`).
- `accounts`: `kind` is `mmio` (one per memory region the machine registers or wraps: its own
  peripherals and the QEMU USARTs, NVIC/SCB, SysTick, PPB default region; count = reads + writes),
  `timer` (one per machine timer name; count = firings; `deadlines` = vCPU slices that ended at this
  timer's deadline), `chardev` (socket and in-process wire paths; `bytes`), `ctl` (control server:
  commands, JSON building, MSP parsing of the FC's bytes), `fdm`, `handoff` (`vcpu.between_slices`:
  the vCPU thread from one slice end to the next slice start, the timers it runs included;
  `vcpu.bql_wait`/`main.bql_wait`: contended BQL acquisitions), `mainloop` (`mainloop.poll`: count =
  iterations, `fds` = fds polled in total; `mainloop.dispatch`: from a poll's return to the next
  poll), `idle` (`vcpu.halt_wait`, `vcpu.realtime_sleep`, `idle.skip`: count = skips, `virt_ns` =
  virtual time skipped), `other`. `thread`: `vcpu`, `main` or `both`. `host_ns` includes the accounts
  nested inside (an MMIO write that transmits a UART byte contains the `chardev` account), `self_ns`
  does not. `sampled: N`: every event is counted, every N-th is timed and its time counts N times
  (the three hottest MMIO regions, `armv7m.nvic`, `armv7m.systick`, `nexus-xr.dwt`: N = 7).
- `handoffs`: one per end of a vCPU slice (`tcg_cpu_exec` returned to the round-robin loop).
  `by_reason`: `timer_deadline` (the budget ran out on executed instructions: a virtual timer is due),
  `idle_skip` (the budget ran out through an idle skip, or an idle-skip halt), `timer_deadline_realtime`
  (a `QEMU_CLOCK_REALTIME` timer is due), `timer_rearm` (the vCPU armed a virtual timer earlier than its
  budget end), `io` (another thread — the main loop — kicked the vCPU: an interrupt or timer from a
  socket/chardev handler, queued work), `halt` (guest WFI), `other`. `by_timer`: for the two deadline
  reasons, which machine timer was due (`not_machine`: none of the machine's — QEMU's own, e.g.
  SysTick). `wake_ns`: the vCPU wrote the main loop's event notifier (`aio_notify`) → the main loop's
  poll returned. `kick_ns`: another thread kicked the vCPU → the vCPU left `cpu_exec` / woke / started
  its next slice. Latencies: count, mean, p50, p99 (±6 %), min, max.
- `mainloop`: poll iterations, host ns inside the poll (sleeping, BQL released) and outside it.
- `threads`: per thread, time in top-level accounts and the rest. On the vCPU thread the rest is
  guest code, TB lookup/translation and QEMU bookkeeping: `guest_exec_or_unaccounted_ns`;
  `accounted_ratio` = accounted / wall of the vCPU thread (a low value means host time no account sees).

## Client → core

### `rc` (transmitter channels; the app maps joystick axes → channels; the core feeds `fb_crsf`)
```json
{"type":"rc","ch_us":[1500,1500,1000,1500,1000,1000,1500,1500,1500,1500,1500,1500,1500,1500,1500,1500]}
```
Missing trailing channels keep their last value. Sent whenever an axis changes, at most ~250 Hz.
Values must be 800..2200 µs. Channels are the transmitter's (CRSF ch1..16); the firmware's `map`
applies (the RS7 profile uses AECR1T23, so MSP_RC reads back roll, pitch, yaw, collective, ...).

### `fault` (inject a device fault)
```json
{"type":"fault","device":"crsf","kind":"link_lost","on":true}
{"type":"fault","device":"crsf","kind":"lq","on":true,"params":{"lq":30,"rssi_dbm":-95}}
```
Applied to the device model; `device`/`kind` are the manifest's (`devices[].id`, `faults[].id`),
`params` its `faults[].params` (required ones when turning on; missing optional ones keep the
normal value). `on` defaults to true and must be a JSON bool. Unknown device or kind, a device this
session has not wired (`fault: device crsf is not wired in this session`), `params` not an object, an unknown param, a missing/non-number/
out-of-range/non-integer param → `error` (nothing applied). Every change is an
`event` `fault` `{device, kind, on}`; `state.faults` lists the active ones. Faults stay on across
firmware resets and power cycles (they are the hardware's) until turned off. Implemented:
- `crsf.link_lost` — the receiver goes silent: no RC frames, no LINK_STATISTICS (`fc.link` up
  false, LQ 0); the firmware goes to RXLOSS/failsafe. Same as `link {"up":false}`.
- `crsf.freeze` — the receiver keeps sending, at the packet rate, the RC frame it had when the
  fault came on; LINK_STATISTICS stay live. The firmware cannot tell by silence: it stays armed
  with frozen channels (`fc.rc_us` constant while `rc_us` moves).
- `crsf.lq` — LINK_STATISTICS carry `params.lq` (%, required), `rssi_dbm`, `snr_db` instead of the
  `link` values. The firmware's RSSI (MSP_ANALOG) is a 16-frame average: it settles in ~3.2 s.
- `icm42688p.freeze` — the IMU's data registers stop updating; DATA_RDY and INT1 keep coming, so the
  driver reads the same gyro/accel values every sample.
- `dps310.freeze` — the baro's result registers stop updating (the chip keeps answering on I2C,
  the driver reads the same pressure every cycle): the firmware's `baro_alt_m` stays put while the
  FDM climbs; off → it follows again within ~0.5 s.
- `msp_osd.link_lost` — the OSD device goes silent: it ignores what the firmware sends (no frame
  messages, `osd.frames` stops) and sends nothing back; the firmware keeps pushing (it does not
  notice). On → one `osd` `link:false`; off → the canvas is announced again and the next frame is
  full (`clear:true`, `link:true`).
- The F405's sensors (wired on the speedybee-f405-wing; the core rejects a fault for a device the
  session has not wired):
  - `spl06.freeze` — as `dps310.freeze` (result registers stop, RDY no longer set, kept across a
    soft reset and a power cycle).
  - `spl06.offset` — `params.offset_pa` (required, -20000..20000) added to the pressure the chip
    measures: a step in the firmware's baro altitude when turned on (+12 Pa ≈ -1 m near sea level).
  - `spl06.detached` — the chip is gone from the bus: every I2C address phase NACKs (iNav's probe
    finds no baro after a reboot; a running firmware sees its reads fail).
  - `ublox.no_fix` — the receiver keeps sending, with fix type 0, gnssFixOK clear and 2 satellites.
  - `ublox.spoof` — the solution is offset by `params.north_m`, `east_m`, `alt_m` (each optional,
    default 0) while velocity stays true.
  - `ublox.drift` — the solution walks away: a random walk of `params.rate_m` (default 0.5) m per
    √s per axis; off → back to the truth at once.
  - `ublox.silence` — the receiver sends nothing and ignores what it is sent; iNav times out after
    1 s, restarts its handshake and finds the receiver again when it is off.
  - `ublox.glitch` — one-shot: the next navigation epoch is `params.jump_m` (default 100) north of
    the truth; `jump_m` 0 = that epoch's first message goes out with a broken checksum instead.
    Turns itself off after that epoch.
  - `qmc5883l.freeze` — the compass's data registers stop updating (DRDY keeps coming): iNav reads
    the same field while the model turns.
  - `qmc5883l.offset` — a hard-iron offset `params.x_mg`, `y_mg`, `z_mg` (chip frame, each default
    0) added to the field: iNav's heading moves (its calibration does not know it).
  - `qmc5883l.noise` — `params.sigma_mg` (required, 0..8000) Gaussian noise per axis instead of the
    configured `noise_mg`.
  - `qmc5883l.detached` — the compass is gone from the bus (its address NACKs): iNav's reads fail.
  - `battery.voltage` — the FC's voltage sensor reads `params.volts` (required, 0..60; the FC's
    ADC reads at most 36.3 V) instead of the pack (which goes on draining as before).
  - `battery.disconnect` — the sense leads are off: the FC reads 0 V and 0 A (the bench keeps the FC
    powered; the pack goes on as before).
- `esc.telemetry_lost` — the ESC stops answering: no bidirectional-DShot eRPM reply (and no KISS
  telemetry frame); the motor runs on. RotorFlight 4.6 then keeps the last RPM it decoded
  (it updates the telemetry only when edges arrive), so the governor regulates on a frozen value.

### `link`
```json
{"type":"link","up":true,"rssi_dbm":-60,"lq":100,"snr_db":10}
```
Missing fields keep their last value. `rssi_dbm` −130..0, `lq` 0..100, `snr_db` −30..30 (integers; the
ranges of the `crsf.lq` fault params), `up` a bool; anything else is an `error` and nothing changes.
`up:false`: the receiver stops sending RC frames (the firmware
goes to RXLOSS/failsafe, as with a real receiver that lost the transmitter). `up` is an alias of the
`crsf.link_lost` fault (`up:false` = on, `up:true` = off, with the `fault` event); `rssi_dbm`/`lq`/
`snr_db` are the receiver's normal values (the `crsf.lq` fault overrides them while on).

### `time` (lockstep control)
```json
{"type":"time","mode":"run|pause|step","realtime":false,"step_s":0.001,"speed":1.0}
```
`realtime:true` asks the core to throttle to wall clock (never faster); `speed` scales the target.
`pause` stops the virtual clock (QEMU `stop`; no `state` while paused), `step` runs `step_s` of virtual
time and pauses again, `run` continues. A `time` without `mode` only changes `realtime`/`speed`.
`speed` 0.001..1000, `step_s` (0, 3600], `realtime` a bool; all fields are checked before any is applied.
When virtual time is ahead of `host × speed`, the physics tick queues a sleep on the vCPU thread (with
the big QEMU lock released, so the control server keeps serving clients); under `-icount` the stopped
vCPU is stopped virtual time. When the machine is slower than the target it re-anchors instead of
catching up later, so it is never faster than asked. Without `-icount` (virtual = host time) there is
nothing to throttle and the core says so in a `log` line.
On the homelab (i7-4790) the full flight loop at `icount shift=2` runs at ~0.39×, so `realtime:true,
speed:1` has nothing to throttle there; `speed:0.25` → 0.250×, `speed:0.1` → 0.100× (measured).

### `env` (world/environment)
```json
{"type":"env","wind_ned":[2.0,0.0,0.0],"gust":0.5,"pressure_pa":89874,"temp_c":22,"home":{"lat":40.1792,"lon":44.4991,"alt_m":0}}
{"type":"env","scenario":"acc-calibration"}
{"type":"env","scenario":{"name":"my-test","steps":[{"at_s":0,"action":"hold","euler_deg":[0,0,90],"rotate_s":2}]}}
```
With the plane FDM, `wind_ned` (m/s, NED, the direction the air moves to, each within ±100) goes
into the airspeed and `pressure_pa` (30000..110000) + `temp_c` (−60..60) set the air density; `home`
`{lat, lon, alt_m}` (each optional: the others stay) is the FDM origin's place on WGS-84: the GPS's
solution is the FDM's NED position around it (default 40.1792, 44.4991 and the baro's field elevation;
a change makes the solution jump there); every field is checked before any is applied; `gust` is not
modelled (a `log` line). With the heli the whole message is logged and ignored.
**`scenario`** (`core/qemu/hw/arm/fb_fdm.c.inc`, any FDM): an environment the core runs
on the FDM, as data. A string names `models/<aircraft>/env/<name>.json`; an object is given inline;
`null` stops the running one (the model stays where it is). A new scenario replaces the running one
and starts at once. An aircraft's `env_default` (the trainer: `env/launch.json`) runs after the
core starts and after `reset aircraft` (`-M env-scenario=<name>` replaces it). Schema (`fdm/include/fb/plane_scenario.h`):
```json
{"name":"launch","description":"free text",
 "launch":{"speed_kmh":50,"duration_s":0.5,"pitch_deg":10,"heading_deg":null},
 "steps":[
  {"at_s":0,    "action":"pad"},
  {"at_s":0.5,  "action":"hold", "euler_deg":[0,10,null], "rotate_s":0.5, "event":"pose", "label":"launch attitude"},
  {"at_s":null, "action":"launch", "event":"launch_command"}]}
```
- `steps` (≤ 32) fire in order; a step fires once the previous one has and the scenario time (s
  since it started) ≥ `at_s`. `at_s` must not decrease; `null` = on command: the step waits for the
  matching client command (today `launch`), and the steps behind it wait too.
- `action`: `pad` (as `reset aircraft`: position to the pad at once, rotate level to heading 0 in
  0.25 s, then free), `hold` (a `constraint` hold: `euler_deg` [roll, pitch, yaw] in degrees, yaw
  `null` = keep the heading, or `quat` [w,x,y,z]; `rotate_s` 0..60, default 1), `release`, `launch`
  (the scenario's `launch` object, overridden by the step's own `speed_kmh`/`duration_s`/`pitch_deg`/
  `heading_deg`, then by the command's fields), `mark` (nothing but its event).
- `event` (optional name), `label`, `index` (integer ≥ 0): reported as `event` `{name, data:{label,
  index, step, scenario}}` when the step completes: a `hold` once the model is still at the pose
  (the IMU then reads it; this is the moment a script starts a firmware calibration step), the
  others when they fire. After the last step: `scenario_end` `{scenario}`.
- Errors name the key (`steps[2].action: need pad|hold|release|launch|mark`, `steps[1]: at_s must not
  go back in time`); nothing is applied.
The trainer's scenarios: `launch` (default; pad, 10° nose up, launch on command), `ground-takeoff`
(pad, free to roll; FWD_ONLY ground), `acc-calibration` (the six still poses of iNav's accelerometer
calibration, `index` = iNav's position: 0 level, 1 inverted, 5 left side down, 4 right side down,
2 nose up, 3 nose down; each held 3 s after it is reached, iNav samples 500 ms).

### `constraint` (hold the model: a hand or a stand; plane FDM, wiring phase 3 as above)
```json
{"type":"constraint","mode":"hold","euler_deg":[0,90,null],"rotate_s":1}
{"type":"constraint","mode":"hold","quat":[0.7071,0,0.7071,0],"rotate_s":1}
{"type":"constraint","mode":"free"}
```
`hold`: the model rotates to the attitude (any: inverted, on a side, nose up) in `rotate_s` (0..60 s,
default 1; 0 = at once, a jump the firmware's estimator does not see, only for a start) with
constant body rates the gyro reports, then sits still at its current position: velocity and rates
zero, the IMU reads the holder's reaction (1 g "up" in body axes), aerodynamics and thrust do nothing.
Exactly one of `euler_deg` ([roll, pitch, yaw] degrees, yaw `null` = keep the heading) or `quat`
([w,x,y,z] body→NED, normalised by the core). A hold replaces a running hold (from where the model
is). Event `pose` `{roll_deg, pitch_deg, yaw_deg}` on the first still step. `free`: let go (free
flight, or free on the ground: the ground rules level it with rates the IMU reads). A scenario's own
`hold` steps report their step event instead of `pose`. Without a plane FDM: `error`.

### `launch` (hand/rail launch; plane FDM, wiring phase 3 as above)
```json
{"type":"launch","speed_kmh":50,"duration_s":0.5,"heading_deg":null,"pitch_deg":10}
```
The FDM holds the model at (heading, `pitch_deg`, roll 0) — `heading_deg` `null` = the current
heading — rotating onto it first if needed (0.3 s), then moves it along body X with the speed
ramping linearly from 0 to `speed_kmh` over `duration_s` and zero body rates, and releases it into
free flight at that speed. During the ramp the IMU reads X = a + g·sin(pitch), Z = −g·cos(pitch),
a = speed/duration; the motion is kinematic (the hand supplies whatever force it takes). Events
`launch` `{phase:"start", a_mss, speed_mps, heading_deg, pitch_deg}` when the ramp begins and
`{phase:"release", speed_mps, height_m}` at release. Missing fields take the running scenario's
`launch` object, else 50 km/h, 0.5 s, 10°, current heading; a scenario step waiting on `launch`
(`at_s: null`) consumes the command. Ranges: `speed_kmh` (0, 360], `duration_s` (0, 10],
|`pitch_deg`| ≤ 60, else `error`. A launch always starts from rest, wherever the model is; a nose-down ramp that reaches the ground
releases there (on the ground, at the speed reached); a
`constraint` `free` during the ramp lets go early.
**Validation:** when a < 1.2 × 18.63 m/s² (iNav's `nav_fw_launch_accel` default, 1863 cm/s²; the
number lives in `fdm/include/fb/plane.h` `FB_INAV_LAUNCH_ACCEL_MSS`) the core applies the launch
and answers with a `log` warning (`launch: a 19.8 m/s^2 < 22.4: iNav's launch detector may not fire`);
for 50 km/h that is `duration_s` > 0.62 s. The default profile gives 29.5 m/s² for 0.5 s: iNav's
bungee detector (IMU X > 18.63 m/s² for 40 ms, tilt ≤ 45°) fires.
The `launch` function (`{"type":"function","name":"launch"}`) is the same with the aircraft's catalog
params under the command's; both report `function` messages (`start`, `step` at the ramp, `done` at
the release).
The pilot socket accepts the same line (`{"type":"launch",...}`, one JSON object per line next to
its channel lines), so a script flying the model can throw it; with `"at"` (virtual seconds) it is
queued with the channel lines and fires at that virtual time (`core/qemu/scripts/flight.py` plane
plans). The core logs each launch (`launch: 50 km/h in 0.5 s at 10 deg (a 27.8 m/s^2, IMU X 29.5 m/s^2)`).

### `reset`
```json
{"type":"reset","what":"aircraft|firmware|all","firmware":false}
```
`aircraft` teleports the FDM back to the pad in a way the IMU can follow: position and velocity jump
at once (a wired GPS reports the jump), the attitude rotates to level and heading 0 in 0.25 s with consistent body
rates, so the running firmware sees a spin to north and its attitude estimator ends level too
(event `reset {what:"aircraft",firmware:false}`, no reboot; the arm state is whatever the firmware
keeps). The plane FDM does the same (`fb_plane_return_to_pad`), then starts its `env_default` (or
`-M env-scenario=`) again. `"firmware":true` also resets the MCU with the flash kept (as `firmware`; events
`reset {what:"aircraft",firmware:true}`, then `reboot`; the arm switch has to be cycled); the app's
Battery / Restart bench is the usual way to reboot. `firmware` resets the MCU (config in flash survives, as
on the real board); `all` also clears the flash config sector (0x08004000, 16 KiB): the firmware then
applies the custom defaults (board config + session profiles), saves and reboots once more.

### `cli`
```json
{"type":"cli","text":"status"}
```
Sends a CLI command over the MSP UART and streams the reply as `log` lines with `src:"cli"`. The core
takes the port out of CLI mode afterwards (`exit` → reboot) only when asked: `{"type":"cli","text":"exit"}`.
While in CLI mode the configurator cannot talk MSP; the core reports that in `fc.cli:true`.
The core enters CLI mode with `#` (followed by a CR, harmless should the firmware already be in the
CLI; sent at the next gap between the host's MSP frames, never inside one), waits for the firmware's
banner and then sends the queued lines;
every line the firmware prints comes back as a `log` (`src:"cli"`, including the echo). `fc.cli` also
turns on when another tool on the MSP socket enters the CLI (the banner is seen; a `#` from that tool
counts only between MSP frames, a 0x23 byte inside a binary frame is data). It turns off at
the next MCU reset: `exit` (always reboots), but also `save` (the firmware reboots after saving) and
`reset`/`power`. The firmware ignores CLI entry while armed; the core then logs that and drops the
queued lines after 1 s (the same when no gap between host frames came within 1 s).
```json
{"type":"cli","exit":true}
```
Leaves the CLI whoever entered it (the app's "exit CLI" button next to its `fc.cli` indicator): the
core sends `exit`, the firmware reboots without saving, `cli {open:false}` follows. Refused (`error`)
when the firmware is not in its CLI (or still entering it), while the config script is applied or
the core reads the `diff`.

### `config`
```json
{"type":"config","reset":"first_start"}
{"type":"config","reset":"defaults"}
```
CLI boards: discards the bench state (both files), erases the config sector and reboots; the whole
first start runs again — the script, the automatic functions — ignoring the bench state and the
snapshot, and its saves make the new bench state (events `config {state:"reset","reset":..}`, then
`applying` ... `applied` as on a first start). `first_start` and `defaults` do the same: iNav and
RotorFlight write their defaults to an erased sector at boot, before the script, so "the firmware's
defaults" is always the first start's starting point (the names differ for the app's wording).
Refused when the firmware is armed or the board is a blob board (`reset all`). A CLI board without a
script: the bench state discarded, the sector erased, the firmware boots with its defaults (which it
writes: the new bench state); only the `reset` event, no `applying`. The script is the bench's as
its files assemble it now (read again as `config apply` does, below: a `bench_cli` the app named
since the start — a snapshot — is in it; a changed `-M` string is logged, restart for it); a bench the
resolver rejects: `error` `config: reset: the bench … is rejected: …; nothing reset`.
`reset all` (above) erases the sector too but restores the first-start snapshot (when there is
one), not the bench state; the snapshot's sector then becomes the bench state.

```json
{"type":"config","apply":true}
{"type":"config","apply":true,"full":true}
```
CLI boards: **apply the script now.** The core assembles the bench again from its files (the bench
JSON, `board_config`, the aircraft's `firmware_cli`, the profiles, the devices, `bench_cli`,
`disabled_lines`: what the app edited; the resolver's notes as `log` lines) and types into the
firmware (the delta, the default):
- the lines the config sector's script does not have — the delta against what was last applied
  (the bench state's / the first start's script: a new or edited line), and
- the lines of every setting **the firmware's config has with another value** than the script's
  (the configurator, the CLI changed it since): the script's value again, each listed in
  `reasserted` (`{setting, script, firmware, layer, source}` as `diff against:"script"`'s `changed`).
  This needs the firmware's config now: when the last `diff all` is stale (a save since) or there is
  none, the core runs `diff all` first (the `diff` message; **the firmware reboots**), then the
  apply; `diff all` not possible (`calibrating`, ...): `error` `config apply: the firmware's config
  is not known ... nothing typed`. A setting the diff does not show (the firmware has its default
  there) is not re-typed: the diff cannot tell a default the script repeats from one the configurator
  set back — `"full":true` for those.

Each line goes with the context lines in force at it in the script (`control_profile N`,
`mixer_profile N`, `battery_profile N`, `rateprofile N`: a line of profile 2 is typed after
`control_profile 2`), and the script's last selection of each context after them. `"full":true`
types every line. Then `diff all` (the `diff` message, `source:"apply"`) and `save` (**the firmware
reboots**). Events: `config` `applying {"apply":true,"delta":true,"lines":N,"sent":[...],
"removed":[...],"reasserted":[...]}`, `applied {"apply":true,"delta":true,...}` (the same members;
or `failed`). Lines no longer in the script are listed in `removed` and not reverted (`config reset`
for a clean start). Nothing to type (the firmware has every line, no setting differs): `applied
{"apply":true,"delta":true,"lines":0}` at once, no reboot. The first-start
snapshot is left alone (it stays the baseline of the script it was made with); the new script is the
bench's from now on (a later first start / `reset` uses it). Only the script: a changed wiring or
device option (the bench's `-M` string) is logged and flagged `"restart_needed":true` — restart the
bench for those. Refused (`error` `config apply: not now: <why>`) while `armed`, `busy` (the first
start, a function, the diff, another apply), `cli`, `power_off`, `not_ready`; `error` for a blob
board, a core without `session=`, a `-M config-cli=` script, a bench the resolver rejects (its first
error; nothing changes), a line longer than the CLI's 63 characters. The bench start's own delta
(the bench state restored, the script's new lines) types the context lines the same way but
re-asserts nothing: there the bench state is the running config.
```json
{"type":"config","snapshot":"bench","path":"D:/…/benches/trainer.cli"}
```
**Snapshot**: the firmware's current `diff all` (the `diff` path: CLI, `diff all`, `exit` — **the
firmware reboots**; refused as `diff` is) written to `path` — the app names the file, the core
writes no file it was not given (a relative path is the data root's). The file: a `#` header (what,
the firmware, when), then the diff's lines without blank lines, `batch`, `defaults`, `save`, `exit`
(comments kept); written atomically (a temporary file renamed over it). Answer to the client that
asked: `{"type":"config","t":..,"snapshot":"written","path":"…","lines":N}` (N = the setting lines) or
`"snapshot":"failed"` + `error` (the diff failed, the file could not be written). The app then names
that file as the bench's `bench_cli` (layer `bench`). **Precedence** after that: the running config
is the bench state's (`-M bench-state=`): at the next start the sector is restored and the
snapshot's lines the bench state's script lacks go as the delta (the same values: nothing changes);
the snapshot is the **starting diff** for a fresh first start — `config reset` (which reads the
bench's files again: no restart needed), `reset all` of a new script, a new bench — which types the
board, aircraft, device lines and then the snapshot's. In short: bench state (the running config) >
the snapshot's lines (the delta at a start; `config apply` types them now) > the first start.
```json
{"type":"config","export":true}
```
**Export as preset**: the core writes nothing; it answers what the app needs to pack the bench and
its layer files: `{"type":"config","t":..,"export":{"bench":"<bench file>","root":"<data root>",
"assembly":<config_json, assembled from the bench's files now>,"script":"<the script text>",
"applied":true|false (the firmware's sector has exactly this script),"diff":{"text":..,"stale":..,
"source":..,"at":..} (the last diff all, null text when none)}}`. The app copies the files of
`assembly.layers` (the board config and profiles by their data-root paths; the `bench` layer's file
next to the preset) and may take `diff.text` as a snapshot.

A `config` message carries exactly one of `reset`, `apply`, `snapshot`, `export` (else `error`).

### `modes`
```json
{"type":"modes"}
```
Reads the firmware's modes again (MSP_BOXNAMES too) and sends `modes` to this client and the
subscribers (works without a subscription; the only MSP traffic a script's core adds). While the
firmware is in its CLI or powered off: `{"type":"modes","error":"cli"|"power_off"}` to this client.

### `diff`
```json
{"type":"diff"}
{"type":"diff","refresh":false}
```
`refresh` (default true): the core enters the CLI on the MSP UART, runs `diff all`, leaves with
`exit` (**the firmware reboots**, as `exit` always does on iNav and RotorFlight: a configurator
connected to the MSP port sees a reboot) and sends `diff` to every client (`source:"request"`); the
modes are read again after the reboot. While its CLI session runs the MSP host socket is not read
(a configurator's bytes wait in the socket, as during the config script) and `cli` is refused.
Refused — a `diff` to this client with the last text (or `null`), `stale:true` and `error` — when
the firmware is `armed` (it ignores CLI entry then), `calibrating` (its status shows the
gyro/ACC calibration running, e.g. the few seconds after a boot), in the CLI already (`cli`: a host tool or the
`cli` command has it; `exit` first), `power_off`, `busy` (the first start or a function runs),
`not_ready` (the firmware has not answered yet); `no_cli` when the CLI did not come within 1 s,
`timeout` when `diff all` did not end within 10 s (then `exit` anyway). `refresh:false`: the last
diff as it is (`error:"none"` when there is none). Never leaves the firmware in the CLI.
```json
{"type":"diff","against":"script"}
{"type":"diff","against":"script","refresh":false}
```
**The script against the firmware**: after the `diff all` (as above; `refresh:false` = the last one)
the core compares the script it sends (the running assembly) with the firmware's diff, setting by
setting (the normaliser of the layers' conflicts, `devices/include/fb/cli_norm.h`; the Python
reference `core/qemu/scripts/cli_norm.py`), and answers the client that asked:
```json
{"type":"diff","t":61.2,"against":"script","source":"request","at":61.1,"stale":false,
 "sent":[{"setting":"set nav_fw_bank_angle","value":"40","text":"set nav_fw_bank_angle = 40","layer":"bench","source":"…/trainer.cli:2"}, …],
 "current":[{"setting":"aux 3","value":"1 3 0 16","text":"aux 3 1 3 0 16"}, …],
 "changed":[{"setting":"aux 2","script":"aux 2 36 2 1700 2100","firmware":"aux 2 1 3 0 16","layer":"aircraft","source":"models/trainer/inav.cli:178"}],
 "only_firmware":[{"setting":"aux 4","firmware":"aux 4 36 2 1700 2100"}],
 "only_script":[{"setting":"set nav_fw_launch_max_angle","script":"set nav_fw_launch_max_angle = 45","layer":"aircraft","source":"…"}]}
```
`sent`: the script's settings as they end (a later line wins, `smix reset`/`mmix reset` clear);
`current`: the diff's; `changed`: in both with other values (the firmware's config was changed —
the configurator, the CLI, a calibration); `only_firmware`: set in the firmware, not by the script
(the configurator's additions, calibration results); `only_script`: the script sets it, the diff
does not show it — the firmware has its default there (the script's value is the default, or it was
set back). `setting` names a context other than 1 as `set roll_rate @control_profile 2`. A diff that
could not be refreshed: `error` (as `diff`) and the comparison against the last text, `stale:true`
(no last text: no comparison). `against` other than `"script"`: `error`. A `log` line gives the
counts.

### `function`
```json
{"type":"function","name":"calibrate_acc"}
{"type":"function","name":"calibrate_gyro","params":{"hold_s":3}}
{"type":"function","name":"calibrate_mag"}
{"type":"function","name":"launch","params":{"speed_kmh":60}}
```
Runs one of the aircraft's environment functions (`manifest` `aircraft[].functions`; what each
does and its progress: the `function` message). `params` override the catalog's (same names and
ranges as there). A name the aircraft does not list, `params` not an object, a bad param, a
calibration while one is refused (armed, in the CLI, powered off, another function or the first
start running, the diff being read) → `error` (`function calibrate_gyro: the firmware is armed`),
nothing started.

### `firmware`
```json
{"type":"firmware","path":"D:/.../rotorflight_STM32F7X2_OSD.elf","watch":true}
```
Loads a new firmware image (ELF: PT_LOAD segments at their load addresses; Intel HEX; or a raw
binary at 0x08000000), erases the whole flash, writes the image plus the session's custom-defaults
blob, and does a power-on reset: the firmware applies the bench's board config as custom defaults.
The file is read by the core, so the path is the core machine's. An unreadable/invalid file is an
`error` and leaves the running firmware alone. `watch:true` polls the file's mtime/size once per
second and reloads after the change has held still for one poll; `watch:false` stops it; a
`firmware` message without `path` only changes `watch` for the current file. `watch:true` re-does that whenever the file changes on disk (edit → make → test
without leaving the app). Progress and errors arrive as `log` lines with `src:"core"`.

### `power`
```json
{"type":"power","on":false}
```
Battery off / on (`on` a JSON bool, default true; anything else is an `error`): the MCU is held in reset / released and boots from what is in its flash — no
diff re-applied, like power-cycling the real board. While off, the peripherals are at their reset
values (the ESC gets no DShot and spins down, the servos hold), the CPU is off, the virtual clock,
FDM and receiver keep running and `state` keeps coming (`fc.power:false`).

### `prof` (host-time ledger; `-M prof=on`)
```json
{"type":"prof"}                           {"type":"prof","dump":true}   one `prof` message to this client
{"type":"prof","reset":true}              the ledger starts again (all counts 0; a `log` line says so)
{"type":"prof","dump":true,"reset":true}  dump, then reset
```
`{"cmd":"prof",...}` is accepted as the same command. A core without `prof=on` answers `error`.

### `subscribe`
```json
{"type":"subscribe","state_hz":30,"osd":true,"log":true,"modes":true,"signals":["fdm.alt_m","out.servo1_us"]}
```
Per-client rate limits for the periodic streams. `state_hz` 0 stops `state`/`samples` for that
client, any other rate is clamped to 0.1..1000 (negative or not a number: `error`); `signals:[ids]`
replaces the list for `samples` (`[]` stops them). `log`/`osd` are JSON bools. Everything is checked
before anything applies: a bad `state_hz`, a non-bool `log`/`osd`, `signals` not a list, an unknown
id or more than 128 ids → `error` and the subscription stays as it was (rate, flags, the previous
signal list). Defaults on connect: 100 Hz, `log:true`, `osd:true`, `modes:false`, no samples.
`modes:true` = the core keeps this client's `modes` current by itself (after each boot and the
configurator's changes, see `modes`); the app turns it on, scripts leave it off. `osd:false` stops
the `osd` messages for that client; turning it on again sends the whole canvas at once.

### `signals` (on connect and when the set changes)
```json
{"type":"signals","items":[{"id":"fdm.alt_m","unit":"m"},{"id":"fc.attitude.roll_deg","unit":"deg"},{"id":"out.servo1_us","unit":"us"}]}
```
Every value that goes into or out of the flight controller has a stable id (105 in the QEMU core:
`fdm.*`, `imu.*` incl. `imu.noisy.*` = the sensor input after noise, `out.*`, `esc.*`, `rc.ch1_us`..,
`link.*`, `crsf.*`, `osd.frames` (DRAW_SCREENs the OSD device received; NaN without it), `baro.pressure_pa`/`baro.temp_c` = what the baro chip is given (standard
atmosphere at the FDM altitude, before the chip's noise; NaN without a baro), `fc.*` from MSP incl.
`fc.baro_alt_m` (MSP_ALTITUDE, NaN until answered), `fc.gps.fix`/`num_sat`/`lat_deg`/`lon_deg`
(MSP_RAW_GPS), `mag.x_g`/`y_g`/`z_g` (the field the compass chip is given, its axes), `gps.*` (what the
GPS receiver sent last), `battery.voltage_v`/`current_a`/`mah_used`/`soc`, `timing.realtime_factor`;
NaN for a device not wired). `state` carries the
common ones; a client asks for extra ones with `subscribe` (`signals:[ids]`, delivered as
`{"type":"samples","t":..,"v":{id:value}}` at `state_hz`).

## Session description (what the app hands the core at start)
A JSON file, generated by the app from the wiring graph:
```json
{"board":"nexus-xr","firmware":"path/to/rotorflight_STM32F7X2_OSD.elf","board_config":"boards/nexus-xr/NEXUS_XR.config",
 "profile":"models/rs7/qemu-profile.txt",
 "aircraft":"models/rs7/aircraft.json",
 "wiring":[{"device":"icm42688p","slot":"gyro1"},{"device":"dps310","slot":"i2c3"},{"device":"crsf","slot":"uart5"},
           {"device":"esc","slot":"motor1","telemetry":"bidir"},{"device":"servo","slot":"servo1","actuator":"swash_a"},
           {"device":"servo","slot":"servo2","actuator":"swash_b"},{"device":"servo","slot":"servo3","actuator":"swash_c"},
           {"device":"servo","slot":"servo4","actuator":"tail"}],
 "environment":{"sea_level_pa":101325,"field_elevation_m":990,"temp_c":22},
 "time":{"icount_shift":2,"realtime":false,"idle_skip":false},
 "ports":{"control":31250,"msp":31235}}
```
Slots are the board's resource names from its unified config; the core resolves them to pins,
timers and buses. Devices not wired are absent (the firmware sees nothing on that bus).

`board` is `nexus-xr` (default) or `speedybee-f405-wing` (stock iNav 9.1.0, target
SPEEDYBEEF405WING; `core/qemu/sessions/f405-boot.json`, `f405-crsf.json`):
```json
{"board":"speedybee-f405-wing","firmware":"~/flightbench-spike/inav-fw/inav_9.1.0_SPEEDYBEEF405WING.hex",
 "aircraft":"rs7","wiring":[{"device":"crsf","port":"uart","pad":"uart1"}],
 "time":{"icount_shift":2,"realtime":false,"idle_skip":false},"ports":{"control":31250,"msp":31235}}
```
It takes only the new wiring form (an old-form `{device, slot}` list is an error); its machine models
the gyro on gyro1, a CRSF receiver on uart1..uart5 (uart6 is the MSP host socket), the ESC on S1
(`output1`) and servos on S1..S11 (`output1`..`output11`), the MAX7456 OSD on SPI2 (`osd1`, onboard, no
video: iNav's driver needs it to boot), an MSP DisplayPort OSD device (`msp_osd`, an HD VTX) on
uart1..uart5 (`-M osd=uartN`; its iNav lines: `feature OSD`, the port's function FUNCTION_MSP_OSD
`serial <n> 33554432 115200 115200 0 115200`, `osd_video_system = HDZERO`, an `osd_layout` for
50 × 18; with it iNav leaves the MAX7456 idle); the SPL06 baro and the SD card are onboard but not modelled
(left out with a note). A bench without `wiring` gets its onboard
devices only: the gyro, no receiver, no ESC, no servos. The machine runs no FDM on this board yet
(`fdm=on` is never set; a warning says the IMU sees a board at rest), has no baro (`environment` is
checked, not applied) and takes its beeper pin from board.json. There is no custom-defaults blob:
the configuration is one CLI script, `config_cli` (below).

The same wiring in the new form (`core/qemu/sessions/rs7-pads.json`): a device **port** on a board
**pad**, the pad by firmware id or printed label, onboard devices implied:
```json
 "wiring":[{"device":"esc","port":"signal","pad":"motor1","telemetry":"bidir"},{"device":"esc","port":"tlm","pad":"TELEM"},
           {"device":"esc","port":"rpm","pad":"RPM"},{"device":"servo","port":"pwm","pad":"servo1","actuator":"swash_a"},
           {"device":"servo","port":"pwm","pad":"servo2","actuator":"swash_b"},{"device":"servo","port":"pwm","pad":"servo3","actuator":"swash_c"},
           {"device":"servo","port":"pwm","pad":"TAIL","actuator":"tail","reversed":false}]
```
- `port` defaults to the device's first port; the other keys of an entry are its props.
- Onboard devices (`boards[].pads[]` with `onboard:true`) are on the board without an entry; an entry
  on their own pad only carries props. `{"device":"icm42688p","detached":true}` removes a
  `detachable` one; a soldered one (`detachable:false`) cannot be removed. On the Nexus-XR the gyro,
  the baro (`dps310`: soldered on the real board, detachable here = a bench without a working baro)
  and the receiver are detachable, the blackbox flash is not.
- The new form (any entry with `pad`, `port` or `detached`) is **complete and strict**: the machine
  has exactly the wired devices plus the onboard ones not detached, and every problem below is an
  error.
- The old form (entries with `slot`: what the app writes today) keeps the behaviour from before the
  wiring model: `slot` is the pad id, the port the device's first; the **receiver** (on uart5, or on
  the modelled UART uart3..uart6 the list puts it on), the **ESC** on motor1 and **servo1..4** are
  present whether listed or not, and the list only adds and configures devices. When one of them is
  not listed, the core logs one line `old-form session: receiver/ESC/servos assumed present (not in
  the list: ...)`. A detachable onboard device the list omits is off: the gyro (the old `imu=off`
  rule) and, since the baro became detachable, the baro — an old-form list without
  `{"device":"dps310","slot":"i2c3"}` gives `baro=off` (a note says so); a soldered onboard device
  it omits stays (warning). An entry the machine cannot place (unknown device or pad, wrong
  pad kind, a UART pad without the pins, a pad already taken: the first entry wins) is a warning and
  left out, not an error. Props and actuators are checked as in the new form.
- A list that mixes the forms (an entry with `slot` next to one with `pad`/`port`/`detached`, or both
  in one entry) is rejected. A session without `wiring` gets the machine's defaults (the RS7 layout:
  gyro, baro on i2c3, receiver on uart5, ESC on motor1, servo1..4) — the baro is on.
- Props: an entry's keys other than `device`/`port`/`pad`/`slot`/`detached` are props of that device
  and must be in its `props` (manifest) with the declared type and range (`int`/`float` min..max,
  `bool`, `enum` options); an unknown key is an error naming the entry. Tolerated on any entry (the
  app's `ToSession` writes them everywhere): `"actuator":""`, `"reversed":false`, `"option":""`;
  on the ESC `option` is its `telemetry`.
- Actuators: only servo and ESC entries take `actuator`. A servo's must be one of the aircraft's
  `fdm_inputs`; the ESC's must be the aircraft's `throttle` (the ESC's output always carries that
  name, it is never renamed); each actuator on one output.
- Errors (the session is rejected): unknown device, port or pad; a port on a pad of another kind; a
  UART port whose direction the pad cannot carry (RX-only `TELEM` for a receiver); two devices on one
  pad (an I2C bus takes several, but not two of one device: same address); removing a soldered
  onboard device; a device both wired and detached; `detached` not a bool; a bad prop or actuator
  (above); mixed forms; a bad `aircraft.json` (below). A valid entry the machine does not model
  (GPS, compass, the ESC's `tlm`/`rpm`, the blackbox flash) is reported and left out.

Given to the core as `-M nexus-xr,session=file.json` (see "Launching the core" below; `core/qemu/run.sh
--session file.json` builds that command line). The machine resolves the bench itself, in C
(`devices/src/session.c`), before anything starts; `core/qemu/scripts/session.py` + `wiring.py` +
`mkdefaults.sh` + `manifest.py` are the reference implementation it is tested against
(`core/qemu/tests/test_session_equiv.sh`). What the QEMU core does with each field:
- `firmware` (ELF/hex/bin), `board_config` and `profile` — a path or a list of paths — plus
  `profile_text` (inline CLI lines, what the app's `ToSession` sends from the pasted `diff all`) form
  the custom-defaults blob (built in memory: the header, the board config, the profiles' lines without
  comments and blank lines, the wired devices' `config` lines; smaller than 6136 bytes, else an
  error). Relative paths are relative to the data root (`root=`); `~` and `~/...` expand to the home
  directory (`HOME`; Windows `USERPROFILE`, then `HOME`); `\` is read as `/`. `"board_config":""`
  boots without a blob.
- On a board configured by CLI (`boards[].config` `cli`: the speedybee-f405-wing) the same inputs form
  one CLI script instead of the blob (`config_cli`), which the machine sends over its MSP UART on
  first boot and then saves: the board's base script (`board_config`, default
  `boards/<id>/defaults.cli` = board.json `defaults_cli`; `""` = none), the aircraft's CLI script
  (`aircraft.json` key `firmware_cli`, a path relative to the data root; ignored on blob boards), the
  `profile` files, `profile_text`, the wired devices' `config` lines — in that order. Each line is
  stripped; blank lines and `#` comments are dropped; `save` and `exit` lines are dropped with a note
  (the core saves at the end); `defaults`, `reboot`, `dfu`, `bl`, `bootloader` and `msc` lines are an
  error (they would reset or wipe the board mid-script). Commands are matched by their first word,
  case-insensitively, as the firmware's CLI does.
- **The script's layers** (a CLI board): every line of the script has a layer and a source —
  `board` (the base script, `boards/speedybee-f405-wing/defaults.cli:12`), `aircraft` (its
  `firmware_cli`, `models/trainer/inav.cli:40`), `profile` (the `profile` files `<file>:<line>`,
  `profile_text:<line>`), `device` (a wired device's lines, `device ublox on uart3`), `bench` (the
  bench's own lines, `bench_cli`: `<file>:<line>`) — in that order, and the fifth, read-only: the
  firmware's current config (`diff all`, the `diff` message; the firmware is its source). A file path
  is shown relative to the data root when it is under it. Each line's **ref** is `<layer>:<n>`, its
  1-based position among its layer's lines (as `config_json` lists them). A ref is positional: it
  moves when a line is added before it in its layer — a device wired before another, a device option
  that adds a line, an edited file — and then names another line (reported: the note and
  `config_disabled:` show the line it matched). For a lasting override prefer the line's exact text;
  a ref is for a line whose text is not unique.
  - `bench_cli` (optional, a path like the others: relative = the data root's): a CLI file of the
    bench's own lines, appended after the device lines — the app writes it (next to the bench);
    the core only reads it (a missing file is an error). On a blob board: an error.
  - `disabled_lines` (optional, a list of strings): each an exact line text (whitespace around it
    ignored) or a ref `<layer>:<n>` (`device:2`); the lines it matches are not sent (reported as
    `config_disabled:` and a `session: config: disabled_lines ...: ... not sent` note), an entry that
    matches no line is a note. For overriding a generated device line without editing the catalog.
    Not a list / not a string: an error. On a blob board: an error.
  - **Conflicts**: the same setting in two or more layers with different values (the rules of
    `devices/include/fb/cli_norm.h`: `set <name>` (the `control_profile`/`mixer_profile`/
    `battery_profile`/`rateprofile` in force, other than 1, is part of the setting — the last one
    switched, the normaliser does not know which setting belongs to which profile kind; RotorFlight
    counts profiles from 0), `feature`/`beeper`/`blackbox X`
    on/off, `serial <i>`, `aux <i>`, `smix`/`mmix <i>`, `servo <i>`, `map`, `resource <name> <i>`, ...;
    values compare token by token, case-insensitively, numbers by value, iNav's `serial <i> +<bit>`
    against the function mask): the lines are sent in layer order and **the later line wins**, as the
    firmware applies them; each conflict is reported (`config_conflict:`, a `session: config:
    conflict: ...: the later line wins` note, `conflicts` in `config_json`). Nothing is dropped or
    merged: the report shows what the firmware will get.
- `aircraft`: the directory name (`models/rs7/aircraft.json` → `rs7`); only `rs7` has an FDM.
- `wiring`: resolved as above (`core/qemu/scripts/wiring.py`). The machine models the gyro on gyro1,
  a CRSF receiver on uart3..uart6 (uart5 = the onboard one; uart1 is the MSP host socket, uart2 =
  `TELEM` is RX-only), the ESC's signal on motor1 (bidirectional
  DShot), servos on servo1..4 and the DPS310/SPL06 baro on i2c3 (address 0x76); exactly the modelled
  devices of the assembly exist (no receiver → the UART stays silent, RXLOSS; no ESC → nobody decodes
  DShot, the motor stays at 0; no gyro → empty SPI1; no baro → 0x76 NACKs, the firmware reports no
  BARO), and `fault` for a device not wired is an error. The MSP DisplayPort OSD device (`msp_osd`)
  goes on uart3..uart6 of the nexus-xr, uart1..uart5 of the speedybee-f405-wing (not on the
  receiver's UART; `-M osd=uartN`), one per session (a second
  `msp_osd` entry is rejected); its descriptor's lines for the board's firmware family
  (`config_firmware.rotorflight`: MSP on that port at 115200, `displayport_msp_serial`,
  `osd_displayport_device = AUTO`, an element layout) are appended to the custom-defaults blob
  after the profiles, only when it is wired (`session.py --check` prints them). A device's CLI
  lines are its `config` (every firmware) then its `config_firmware[<family>]`, the family being
  the board's `firmware_family` in `board.json` (`rotorflight` when absent; the
  speedybee-f405-wing is `inav`). Servo `actuator`/`reversed` and the ESC's
  `actuator` name the entries of `actuators` (a servo output without one takes the FDM input of its
  position, `swash_a`..`tail`, unless another output has it); the FDM takes each of its inputs from
  the output wired to that actuator, mirrored around that input's `centre_us` (2·centre − µs; 1500 by
  default, i.e. 3000 − µs) when `reversed`. An FDM input nothing is wired to holds 1500 µs (the FDM's
  neutral). The gyro's `gyro_noise_dps`/`accel_noise_g` set the IMU input noise (props in the
  manifest); `accel_offset_mg`/`accel_scale_pct` give its accelerometer a static error, the same on
  every axis (the chip reads accel · (1 + pct/100) + mg/1000; default none, `-M imu-accel-error=`).
  That error is a property of the bench, like a real chip's: a 6-position calibration measures and
  removes it (the trainer bench has 20 mg / 0.5 %; iNav takes an exactly ideal sensor, accZero 0 and
  accGain 4096, as never calibrated, `sensors/acceleration.c accSetCalibrationValues`).
- `aircraft`: its `aircraft.json` also carries the FDM's parameters (`fdm_params`, the model's nested object
  of `docs/fdm.md`), the ESC and transmission parameters, the FDM's input names (`fdm_inputs`, 4 for both
  types; they must be the model's own: the heli's swash servos rear, front-left, front-right and the tail), the
  ESC's actuator name (`throttle`) and `centre_us` per FDM input (integer 1000..2000, default 1500); the core
  loads them from there. The file is checked before start (and again by the machine): a wrong type or
  out-of-range value is an error naming the key (`esc.kv` > 0, `esc.poles` even 2..64,
  `transmission.main_rotor_gear_ratio` 2 numbers > 0, unknown keys in `esc`; `fdm_params` an object whose
  contents the model itself checks at the session-check, naming the parameter by its path), and the heli FDM
  needs the transmission.
- `environment` (optional) `{"sea_level_pa":101325,"field_elevation_m":0,"temp_c":22}` (these are
  the defaults; ranges 30000..120000, −500..9000, −60..60) → `-M baro-env` (full double precision):
  the baro sees the ICAO standard atmosphere at h = field elevation + FDM altitude. `sea_level_pa`
  and `temp_c` are **sea-level** values (QNH and the temperature at 0 m, not at the field): the
  chip's temperature is `temp_c` − 6.5 K/km · h, e.g. 22 °C and a 990 m field → 15.6 °C on the pad.
- `time.icount_shift` → `-icount shift=N,sleep=off` on the command line (QEMU's, not a machine
  option: the launcher puts it there; the machine warns when the command line differs);
  `time.realtime` → start throttled (`-M realtime=on`, set by the machine itself).
- `time.idle_skip` (bool, default false; needs `icount_shift`) → `-M idle-skip=on`: the core skips
  the firmware's idle polling ("external WFI", `core/qemu/README.md`); optional `time.idle_skip_n`
  (2..1000, default 16) and `time.idle_skip_gran_us` (1..100000, default 50).
- `ports.control`, `ports.msp`: the launcher's `ctl=` and MSP UART socket ports (command line; the
  MSP UART is the board's `msp_uart`: USART1 on the nexus-xr, USART6 on the speedybee-f405-wing). The
  beeper pin is read from the board config / profile text (a CLI board: board.json `beeper`).

## Launching the core
The core is one process, `qemu-system-arm` with the machine of the bench's board (`nexus-xr`,
`speedybee-f405-wing`: the report's `machine=`, below); it reads the bench itself (wiring, device
options, aircraft/FDM, custom-defaults blob or CLI script, manifest). A client (the app; `run.sh
--session` for headless/CI) starts it with this command line and nothing else - no shell, no
python, no temp files:
```
qemu-system-arm -M <machine>,session=<bench>,root=<dir>,ctl=<host>:<control>[,prof=on] -nographic
  -icount shift=<S>,sleep=off
  -chardev socket,id=u1,host=<host>,port=<msp>,server=on,wait=off -serial chardev:u1
  -monitor tcp:127.0.0.1:<monitor>,server=on,wait=off
  -chardev socket,id=fbmotion,host=127.0.0.1,port=<motion>,server=on,wait=off
  -chardev socket,id=fbpilot,host=127.0.0.1,port=<pilot>,server=on,wait=off
```
| part | value | why it is on the command line |
|---|---|---|
| `-M <machine>` | the launch line's `machine=` (`nexus-xr` when absent) | the QEMU machine type of the bench's board |
| `session=<bench>` | the bench JSON file | machine option: the machine resolves it before init |
| `root=<dir>` | the data root: the directory with `boards/` and `models/` (the install dir; the repo root in a checkout) | relative paths in the bench resolve against it. Optional: default `$FB_ROOT`, else the nearest ancestor of the bench's directory that has `boards/<board>/board.json` (a bench in `<root>/<dir>/` finds it two levels up), else the current directory |
| `ctl=<host>:<control>` | `127.0.0.1` (or a tailnet address), `ports.control` (default 31250) | the listen address is the launcher's choice (which interface); without `ctl=` there is no control server |
| `prof=on` | optional | the host-time ledger (`prof`) |
| `bench-state=<file>` | CLI boards, optional: a file per bench, e.g. `<cfg-cache>/<bench id>.state.bin` (`run.sh --session`: the bench file's name) | the firmware's config as the bench left it, written after every firmware save, restored at start (`event config`, above); `<file>` minus `.bin` + `.json` holds the metadata. Without it the bench starts from the first-start snapshot each time |
| `-nographic` | always | no display; stderr/stdout are the core's log |
| `-icount shift=<S>,sleep=off` | `time.icount_shift` (default 2); left out when it is `null` | a QEMU accelerator option, configured before the machine exists: it cannot come from `session=` |
| `-chardev socket,id=u1,...,port=<msp>` + `-serial chardev:u1` | `ports.msp` (default 31235) | the board's MSP UART (MSP/CLI, the configurator; USART1 nexus-xr, USART6 speedybee-f405-wing); a QEMU chardev, the machine takes serial 0 |
| `-monitor tcp:127.0.0.1:<monitor>` | 31236 (not in the bench) | QEMU's HMP monitor (evidence scripts, `qom-get`) |
| `-chardev socket,id=fbmotion,...` | 31237 (not in the bench) | the IMU motion source; the machine finds the chardev by id |
| `-chardev socket,id=fbpilot,...` | 31238 (not in the bench) | the pilot (CRSF channels); found by id |

- **Realtime**: a bench with `time.realtime: true` changes no flag: the machine sets its own
  `realtime=on` from the bench, the command line keeps `-icount shift=<S>,sleep=off` (the throttle
  paces virtual time against the host clock inside the machine). `time.idle_skip` likewise comes from
  the bench (`idle-skip=on`, needs `-icount`).
- **Paths**: native paths (`C:/...` or `C:\...` on Windows, UTF-8); in a `-M` value a comma is
  written `,,` (QEMU splits `-M` at commas).
- **Validate first** (what the app does before starting): `qemu-system-arm -M
  nexus-xr,session=<bench>,root=<dir>,session-check=on -display none` resolves the bench, prints the
  report on stdout and exits 0, or prints `session: <error>` lines on stderr and exits 2. The report:
  the assembly (one line per device port: `esc.signal       -> motor1  (S5)       modelled {...}`),
  `machine: ...`, `config: ...` (device CLI lines), then `root:`, `firmware:`, `board:`,
  `board_machine: <machine>`, `board_config:`, `aircraft: <id> <file>`, `profile:` (per file),
  `profile_text: N bytes`, `options:` (the -M string the bench sets), `icount: shift=<S>,sleep=off` (or
  `icount: none`), `realtime: on|off`, `ports: control=<N> msp=<M>`, `launch: ...` (below),
  `manifest: <one line>`, then the CLI script of a CLI board: `config_cli: N lines` followed by N
  lines `config_cli| <line>` (`config_cli: none` on a blob board or without one), then the script's
  layers (a CLI board): `config_src: [<layer>:<source>] <line>` for every line sent, in the script's
  order, and `config_disabled: [<layer>:<source>] <line> (disabled_lines '<entry>')` in its place for
  a disabled one; `config_conflict: <setting>: [<layer>:<source>] <line> -> [<layer>:<source>] <line>
  (the last wins)` per conflict; then one line `config_json: {...}` (every board) for machines:
  ```
  config_json: {"kind":"cli","layers":[{"layer":"board","file":"boards/speedybee-f405-wing/defaults.cli","path":"/…/boards/speedybee-f405-wing/defaults.cli"},
   {"layer":"aircraft","file":"models/trainer/inav.cli","path":"…"},{"layer":"bench","file":"/…/bench/trainer.cli","path":"…"}],
   "lines":[{"text":"set model_preview_type = 14","layer":"aircraft","source":"models/trainer/inav.cli:15","ref":"aircraft:1"}, …,
            {"text":"set nav_fw_bank_angle = 40","layer":"bench","source":"/…/bench/trainer.cli:2","ref":"bench:1"}],
   "disabled":[{"text":"set gps_ublox_nav_hz = 10","layer":"device","source":"device ublox on uart3","ref":"device:4","by":"device:4"}],
   "conflicts":[{"setting":"set nav_fw_bank_angle","lines":[{…"ref":"aircraft:11"},{…"ref":"bench:1"}],"wins":"bench:1"}]}
  ```
  (one line; `kind` `cli` | `blob`; `layers` = the files read, in order; on a blob board `lines` are
  the device lines of the blob, `layers` its board config and profiles, no conflicts), and last
  `defaults: N bytes (max 6136)` followed by exactly the N blob bytes (or `defaults: none`). The
  older `config: <line>` lines (after `machine:`) stay as they were: the wired devices' lines with
  their `# <device> on <pad> (device config)` headers, disabled ones included.
  What the command line above needs from the bench is one machine-readable line, before `manifest:`:
  ```
  launch: icount_shift=2 realtime=off control=31250 msp=31235 machine=nexus-xr
  ```
  space-separated `key=value`, values without spaces; `icount_shift=none` means no `-icount`
  (`time.icount_shift: null`). A parser reads the first line starting `launch: ` (stop at `manifest: `:
  the blob after `defaults:` is free text) and ignores keys it does not know (keys may be added,
  never renamed). `realtime` is informational (the machine sets it itself). The older `icount:` and
  `ports:` lines carry the same values. Reference parser: `core/qemu/run.sh` lines 63-82 (read the whole report: it can be larger than a pipe buffer). Without
  `session=`, `session-check=on` prints only `manifest: ...` (the default board config). `machine` is
  the QEMU machine type to start (`-M <machine>,...`); the check itself may run on any machine type
  (the resolver does not depend on it), `run.sh` runs it with `-M nexus-xr`.
- **Check without sockets**: run the check with only `-M ...,session-check=on -display none`. QEMU
  opens `-chardev`/`-monitor` sockets before the machine exists, so with them on the check's command
  line their ports are bound (and a port in use fails with QEMU's exit 1, not 2). The check never
  opens the firmware: a missing or unreadable `firmware` passes the check and fails at start
  (`qemu-system-arm: <path>: Failed to open file: ...`, exit 1).
- **Which parts are required**: `session=` (or `fw=` for a bare core), `-nographic` and, with a
  bench whose `icount_shift` is not null, `-icount` (without it the machine warns and runs unpaced,
  not deterministic). Everything else is optional: without `ctl=` there is no control server; without
  the `fbmotion`/`fbpilot` chardevs there is no motion/pilot socket (the machine looks them up by id
  and runs without them). With `-nographic` an omitted `-serial` or `-monitor` goes to stdio (QEMU's
  default `mon:stdio`), mixing MSP bytes into the core's stdout: give `-serial null` / `-monitor
  none` when not using them.
- **Exit codes**: 0 (check passed, or the core ended normally), 2 (bench rejected: `session:` lines on
  stderr), 1 (anything QEMU itself rejects: a bad `-M` value, a port in use, a missing firmware).
  stdout carries only the report (check) or nothing the launcher parses (run); every diagnostic of
  the resolver is a `session: ` line on stderr.
- **Start**: a rejected bench prints `session: <error>` lines and exits 2 before the control server
  listens; warnings (entries left out, old-form defaults, no beeper pin, ...) are `session: ` lines on
  stderr, then `session: <bench> -> <firmware>; <options>`. The blob and manifest stay in memory
  (a `firmware` reload places the same blob again).
- **Debugging**: an explicit `-M` option wins over the bench (`-M nexus-xr,session=b.json,baro=off`
  logs `session: -M baro given: the bench's baro=on is not applied`); `fw=`, `defaults=<file>` and
  `manifest=<file>` replace the bench's firmware, blob and manifest.
- **Stop**: end the process (SIGTERM / TerminateProcess) or `quit` on the monitor.

## Notes
- JSON is fine at 100 Hz state; if the app ever needs more (e.g. 1 kHz plots), add a binary
  `state` frame later — do not optimise now.
- The app must tolerate the core being remote (another machine, WSL2): no file paths in `state`.
- The core is the only writer of virtual time; clients never sleep on their own clock to pace it.

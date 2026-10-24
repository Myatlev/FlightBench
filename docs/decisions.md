# Decisions

Short records of the choices that shape FlightBench. Newest last.

## 2026-09-25 — Emulate the real MCU instead of a SITL target
Context: the RotorFlight POSIX SITL cost a day on five "simulator behaves differently from the board"
issues, all inside the rewritten `target.c`. Decision: run the unmodified target binary on an
emulated Cortex-M. Verified: the `STM32F7X2_OSD` build boots in Renode, writes its custom defaults,
resets, and runs the scheduler.

## 2026-09-25 — Renode now, QEMU as a time-boxed spike
Measured on a Ryzen 7 5800X (see `theory-check-2026-09-25.md`): QEMU's engine is ~6× faster on code
and 3–7× cheaper per peripheral register access, but has no STM32F7 machines, read-only flash on
its F405 machine, no DMA/I2C/DWT. Renode has all of that out of the box and already runs our
firmware. Decision: main line on Renode; a background spike tries to bring a QEMU F722 machine to
"boots to scheduler, CLI over UART". Switch trigger, named in advance: hand-flying hits the
real-time ceiling, or Renode's Windows MMIO cost (1.6 µs/access) hurts app users.

## 2026-09-25 — Device models in an emulator-independent C library
So that a backend change never rewrites the sensors, ESC, receiver, GPS, OSD or protocol code.
Renode peripherals (C#) call the library through P/Invoke; a QEMU backend links it directly.

## 2026-09-25 — Godot .NET app is the front door, headless core is mandatory
Developers launch one application; it starts and configures the core. CI, servers and autotests
use the core alone.

## 2026-10-01 — GPL-2.0-or-later (supersedes the GPL-3.0 choice below)
The core's machine (`core/qemu/hw/arm/*`) is compiled into `qemu-system-arm`. QEMU as a whole is GPL-2.0 and
parts of it are GPL-2.0-only, so a GPL-3.0-only FlightBench could not legally distribute its own core binary:
GPL v2 and v3 are not compatible with each other. GPL-2.0-or-later keeps the copyleft the project wants (forks
stay open) and is compatible with QEMU, with the GPL-3.0 firmware and font assets the app ships (executed and
displayed, not linked), and with any later GPL version. The machine files already carried
`SPDX-License-Identifier: GPL-2.0-or-later`; `LICENSE` is now the GPL v2 text and the README states "or later".
Relicensing later needs the consent of every copyright holder of the code at that time; while the project has one
author that is one person, so contributions are accepted under the same terms to keep it simple.

## 2026-09-25 — Monorepo, GPL-3.0
Core and app versions stay in sync. GPL keeps the tool and community-contributed boards/models open.

## 2026-09-25 — The bench does not paper over firmware behaviour
If a firmware refuses to arm with a configurator attached, the bench shows exactly that. Fixes
belong to the firmware (our fork, for our board).

## 2026-09-25 — Stock scheduler behaviour first; `WFI` idle kept in reserve
The firmware's busy-wait on time registers is the known emulation ceiling (0.12× real time idle on
Renode). We still start with stock behaviour and keep an emulator-only `WFI` idle as a known move.

## 2026-09-25 — Three description files: aircraft (core), visual (app), wiring (session)
One aircraft can be flown with different boards and firmwares by changing only the wiring file.

## 2026-09-25 (evening) — QEMU becomes the main backend
The time-boxed spike passed all three acceptance criteria in one 25-minute iteration: an STM32F722
`nexus-xr` machine (one C file on QEMU v11.1.1) boots the unmodified firmware to its scheduler and
serves the CLI on USART1. Measured idle-loop speed at 250 virtual MIPS: 0.50× on an i7-4790, 0.69×
on a Ryzen 5800X; Renode on the same 5800X: 0.12×. The platform we feared never finishing already
exists, so the main line moves to QEMU now, before any C# peripheral is written. Renode stays as a
cross-check reference (`core/renode/`). The device library plan (`devices/`, plain C) is unchanged
and now links directly into the machine. Details: `speed-2026-09-25.md`, `../core/qemu/README.md`.

## 2026-10-23 — The Transmitter window is the radio; switch types per bench; RotorFlight default
Context: on the v0.1.0 release build the RS7 preset could not take off from the keyboard. RotorFlight's map is
`AECR1T23`: channel 6 is the throttle axis the governor runs off, not a mode box; the firmware's mode table has a range
only on ch5 (ARM), so once the modes arrived the Transmitter window showed one derived switch and nothing could put
ch6 at 2000 (`flight.py --plan liftoff` flew only because it writes ch6 itself). Decisions:
1. The Transmitter window is the pilot's radio, not the firmware's mode table: the radio has switches the firmware
   does not know about; the firmware reads channels. The 2026-10-01 decision stands for *modes* (read-only, written
   only by the configurator); it never meant "a channel exists only if the firmware names a mode on it".
2. Every channel 5..16 has a type, stored in the bench (`transmitter`; the meaning of a channel belongs to the
   aircraft, the joypad and the keys to the user): `auto` (a switch derived from the firmware's modes, no row until the
   firmware gives ranges), `switch2` (1000 / 2000), `switch3` (1000 / 1500 / 2000), `switch6` (ArduPilot's flight-mode
   channel, `RC_Channel::read_6pos_switch`). A static switch has a label and a start position; the firmware's modes
   label its positions, never rebuild it.
3. A bench on a RotorFlight board gets ch5 `auto`, ch6 `switch2` `throttle` (high = throttle on, low = throttle hold);
   other families all `auto`. The RS7 presets carry it explicitly.
4. Keys 5..9 step the Transmitter rows in channel order (RS7: 5 = ARM, 6 = throttle). A position is sent only on a
   click / key.
5. The old `ArmChannel` / `ModeChannel` / `RescueChannel` settings and the pre-modes ARM / mode / third toggles are
   gone; the `keyboard` source stays as it is for now.
6. (evening) A static position the firmware does not name reads `low | high` / `low | mid | high` / `1..6`, not
   `— | —`: on a radio switch "—" told the pilot nothing; `—` stays the derived switches' "no box here".
7. (evening) A bench saved without the `transmitter` block is shown and driven with its board's family default
   (`TxModel.Effective`; the file is written only when the model is edited), old `switches` names becoming labels on
   top. Reason: without it every bench copied from the RS7 preset before the change kept the original bug until edited
   by hand; the family default is not "old switch data", so the migration rule above still holds, and a bench file
   round-trips unchanged.
The live check now flies the take-off through the keys (`app/evidence/tx-model-2026-10-23/`).

## 2026-10-23 (later) — The keyboard is a layer over the joypad, not a device
Context: the keyboard's role was implicit and spread over three rules nobody could see — channels 1..4 fell back to the
keyboard only when no joypad was plugged in, the `keyboard` channel source meant something only on channels 1..4, and the
stick keys (arrows, W / S, A / D) were hard-coded and listed nowhere while every other function lived in Settings → Keys.
Found the same day: with an Xbox pad plugged in, W did nothing and the RS7 could not take off from the keyboard; the live
check had to run with `FB_NO_JOY=1`. Most tests are driven from the keyboard (technical execution of actions), so it must
work whatever else is plugged in. Decisions:
1. The keyboard is a layer, always on (except while a text field has the keys), in parallel with the joypad. Not a device,
   not a channel source; nothing is substituted — with no joypad the keys are simply the only thing moving the channels.
2. Channel sources are `none | ui | axis n | button n | buttons n,m` (the joypad or the Transmitter window only). The
   `keyboard` source is gone; an old `settings.json` with it reads as `none` (one log line).
3. Two hands on one channel: the last input that changed it holds it. A joypad axis takes it when it moves more than
   `JoyTakeUs` from where the pad was when it last held it (20 µs here; 100 µs since 2026-10-24, see below); a key when
   pressed (and when released while it still holds it). The collective
   integrator (W / S) continues from the channel's value when it takes it from the pad. Shown, not hidden: the
   Transmitter window's bars mark who last drove each channel (`kb` / `joy` / `ui`), and its hint line is generated from
   the bind table.
4. Settings → Keys is the complete catalogue in three groups — Sticks (`stick_roll/pitch/collective/yaw_minus/plus`,
   defaults ← → ↓ ↑ S W A D), Transmitter switches (`switch_1..5`, named after the bench's rows when opened over a bench),
   Application — all through the same table and the same `Keybinds.Match` dispatch.
5. Settings → Transmitter states the joypad as a fact (`Joypad: <name> — connected` / `— not plugged in (channels from the
   keys and the Transmitter window)`); the Mode 1..4 picker stays (it only places the sticks in the drawing).
6. `FB_NO_JOY` stays as a dev knob for deterministic runs; the live check passes without it with the pad plugged in (the
   regression test for the bug) and with it (`app/evidence/kb-layer-2026-10-23/`). This replaces item 5's "the `keyboard`
   source stays as it is for now" of the entry above.

## 2026-10-23 (evening) — Inverted flight must be the same physics upside down
Context: the owner rolled the RS7 inverted and gave full negative collective; the Aircraft window showed a climb of
+2.65 million m/s and 1.38 million m of altitude within seconds, with the rotor itself normal (−24°, −245 N, 1799 rpm).
Cause: the helicopter model of the time damped the vertical speed with a coefficient that lost the sign of the rotor
axis when the model was inverted, so the damping became a drive and the vertical speed grew as e^(2.25 t).
Decision: a flight model may not contain a vertical-speed coefficient at all; climb and descent damping has to come
out of the rotor's inflow, so that inverted flight is the same equations with the thrust sign flipped. The model of
2026-10-24 (below, `docs/fdm.md` §2) does that: `fdm/tests/test_heli.c` claim 3 flies an inverted hover and a full
negative-collective climb and expects the climb within 20 % of the upright one.

## 2026-10-24 — The joypad takes a channel on a deliberate move (100 µs); hot-plug stays as it is
Context: the keyboard-layer review asked two things. A stick without a deadzone (a worn pad, not the Xbox one, whose
sticks rest at exactly the centre) drifts 25 µs over a minute and, at the 20 µs threshold, silently takes the
collective from W mid-climb. And a pad plugged in during a flight takes its channels even while a key holds them, so
the collective jumps to wherever the stick is.
Decision (owner): `RcInput.JoyTakeUs` = 100 µs, a tenth of the travel: the pad takes a channel back only on a
deliberate move, measured from where it was when it last held the channel (not frame to frame, so a slow intended
move still gets there); a key takes a channel immediately, as before. Hot-plug stays as it is: this is a development
bench, a restart is a keystroke, and the people plugging a pad in mid-flight are developers; a rule for it is not
worth its code. Test: the `radio` self-test drifts the pad 75 and 100 µs (the keys keep the channel), then moves it
150 µs (the pad takes it).

## 2026-10-24 — FlightBench's own flight models, from first principles; no ported simulator code
Context: both flight models were ports: the fixed wing a line-by-line transliteration of another simulator's plane
model, the helicopter a port of a Python model whose rotor-speed block carried that simulator's constants. That code
is GPL-3.0-or-later; the FDM is compiled into `qemu-system-arm`, which links GPL-2.0-only sources that no device
configuration removes. Options weighed: a rewrite, the FDM out of process over the motion socket (lockstep IPC at
1 kHz, slower, worse determinism), a licence grant from the upstream project, leaving it.
Decision (owner): write FlightBench's own typed FDM for the helicopter and the plane from physics and the standard
references (`docs/fdm.md` names them), with our own parameters named for what they are, and drop the ported code and
its parameter sets entirely. Reasons: an FDM is one piece of the puzzle and more of them will be needed for more
aircraft, so the project needs its own; a model whose parameters are geometry (radius, chord, blade count, head
stiffness) transfers to the next helicopter by its data sheet; and the licence of the whole core must be true.
Consequences: `fdm/` rewritten behind one interface (`fb/fdm.h`), the machine glue is one file for every type
(`core/qemu/hw/arm/fb_fdm.c.inc`), `aircraft.json` → `fdm_params` is the model's nested object and the resolver no
longer knows the parameter names (the core builds the model at a session-check and names a wrong parameter), the
SITL-era Python tools left the repository, the evidence that depended on the old models is regenerated. The release
waits for it; the public history is squashed to the first public commit again so nothing of the ported code remains
in it.

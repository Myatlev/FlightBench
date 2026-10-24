# Screenshots

The shot list for the docs' images. PNG, no fixed size; crop to the relevant window rather than the
whole desktop unless noted. Every v0.1 shot is taken on the **RS7 · First flight** bench — the only
bench the release ships.

## In the docs

| file | where it is used | what it shows |
|---|---|---|
| `hero.png` | README.md | Hero shot: the RS7 just airborne with the windows that show what the bench does (owner's choice 2026-10-23: the first image must show the functionality, not an empty grid). Status `ARMED · ACRO`, Aircraft with head rpm / thrust, Log / CLI with the key presses and `event armed`, Signals plotting the spool-up (`rpm`, `fdm.alt_m`, `euler_deg.pitch`), Transmitter with `ch5 ARM`, `ch6 throttle high` and the owner marks. Retaken 2026-10-23 on the v0.1.0 build: the live check's airborne frame (`FB_LIVE_CHECK=1 FB_CAMERA=orbit FB_SIGNALS=rpm,fdm.alt_m,euler_deg.pitch`, bench layout with `Log / CLI` h=240 and `Signals` open at 420,600,780×260 so the aircraft sits between them). |
| `main-menu-first-run.png` | quickstart.md | Main menu on a fresh install: no recent benches, the Presets list on top, "Start with a preset" hint visible. |
| `status-boot.png` | quickstart.md | The Status window mid-boot: link line `connected`, firmware line `waiting for it to answer (booting…)`. |
| `rs7-liftoff.png` | quickstart.md | Flight screen, RS7 airborne: Transmitter window with the switch row and sticks, Aircraft window with rotor rpm/thrust. Retaken 2026-10-23 from the final live check on the v0.1.0 zip (the airborne frame: `ch5 — / ARM`, `ch6 throttle low / high`, owner marks on the channel bars, the generated keys hint). |
| `configurator-connected.png` | quickstart.md | RotorFlight Configurator 2.3.0 connected to the bench over `tcp://127.0.0.1:31235` (Status tab: board NEXUS_XR, firmware 4.6.0, the Receiver bars following the bench's channels), side by side with the FlightBench flight screen on the same RS7 bench (armed, throttle switch high, head 1799 rpm). Taken by the owner 2026-10-23 on the v0.1.0 build; scaled from 2553×1388 to 1600 wide. |
| `wiring-graph.png` | firmware-dev.md | The wizard's Wiring step on the RS7 bench, a device selected in the inspector. |
| `cli-window.png` | firmware-dev.md | The Log / CLI window in CLI mode, "in the CLI — MSP blocked" and **exit CLI (reboots)** visible. |
| `signals-plot.png` | firmware-dev.md | The Signals window: a few signals plotted on a shared time axis during a flight. |
| `config-window-layers.png` | (not referenced yet; firmware-dev.md → config layers) | The Config window's **Script ↔ Firmware** tab on the RS7 bench: the layered view with its conflicts on the left, the firmware's live `diff all` on the right, the Functions window below. Retaken 2026-10-23 from the release build's `FB_LIVE_CHECK=1` run (its `-views.png` frame). |

## To take

| file | where it goes | what to capture |
|---|---|---|
| `config-window-changes.png` (retake on RS7) | firmware-dev.md → live diff | The Config window's **Changes** tab: a setting changed by the configurator. The current file shows the iNav trainer bench. |

## Kept for a later release

| file | what it shows |
|---|---|
| `osd-overlay-fpv.png` | The FPV camera with the OSD overlay — for when an OSD preset ships. |

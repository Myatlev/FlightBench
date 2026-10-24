# Sensors + the configurator preset on Linux (main `ab6d427`, 2026-09-29)

The Linux baseline for the merge the Windows run measured (`../windows-2026-09-29/`). Homelab (i7-4790,
Ubuntu 24.04, gcc 13.3, glibc 2.39), QEMU 11.1.1 + this tree built by `core/qemu/build.sh` in
`~/flightbench-spike/qemu-prof`. Driver: `scripts/sensors_evidence.py "C L R"` with `PORT_BASE=31290`, an empty
cfg-cache in a temp dir and a temp `XDG_CACHE_HOME`.

| file | what |
|---|---|
| `sensors-evidence-2026-09-29.log` | stages C (first start), L (plane-launch ×2 + report), R (RS7 ×2) |
| `plane-launch-2026-09-29-run1.csv` | the plane-launch FDM log (run 2 byte-identical) |
| `tests-2026-09-29.log` | `make -C devices test`, `make -C fdm test`, `make -C core/qemu/tests test`, `equiv` / `equiv-host` 250/250 |

| | Linux | Windows core |
|---|---|---|
| first start | 134 lines, acczero 41 41 41, accgain 4076 4076 4075, magzero −2 1 0, maggain 1497 1508 1499, applied 72.889 s | the same |
| **plane-launch** | **`51d4a9212465`** ×2 | `c66d93c7274e` ×2 |
| motor start | 24.17 s | 24.18 s |
| climb plateau | 14.3° / 3.83 m/s at 13.9 m/s | 14.3° / 3.82 m/s at 13.9 m/s |
| iNav ends the launch | 32.28 s | 32.38 s |
| level 37..54 s | pitch −0.7 ± 0.5°, 13.0 ± 0.2 m/s, climb 0.10 | the same |
| battery at 55 s (iNav / model) | 16.69 V 6.89 A 79 mAh / 16.693 V 6.881 A 79.9 mAh | 16.69 V 6.89 A 79 mAh / 16.693 V 6.882 A 79.9 mAh |
| GPS vs FDM in flight | mean 1.46 m, max 2.97 m | mean 1.46 m, max 2.97 m |
| **RS7 liftoff** | **`c5a2b9bb6c49`** ×2 (the baseline holds) | `6d8d378c099f` ×2 |

The logs first differ at 4.26 s in iNav's attitude on the pad, and the physics only after the firmware's commands
differ: `../../determinism/README.md`.

A first attempt at this run was discarded. Another session's clients shared ports 31280..31284, reached this
core during stage C and sent it its own pilot plan. Its plane-launch log turned out byte-identical to this
run's over the hashed window, but the run was repeated anyway on 31290..31294.

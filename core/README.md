# core/ — headless bench core

The core is a QEMU build with FlightBench's board machines and device models: `qemu/` holds the machines
(`nexus-xr` for the STM32F722, `speedybee-f405-wing` for the STM32F405), the patches against upstream
QEMU v11.1.1, the session / control-server code, the scripts and the evidence of every measured claim.
See `qemu/README.md`; the build chain for a release is in the top-level `README.md` → *Building from
source*. Device behaviour (sensors, receiver, ESC, servos) lives in `devices/`, flight dynamics in `fdm/`.

The project started on Renode and moved to QEMU after a time-boxed spike (`docs/decisions.md`,
2026-09-25); nothing of the Renode line remains in the tree.

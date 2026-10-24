#!/usr/bin/env python3
"""REFERENCE IMPLEMENTATION: the core resolves benches itself (-M nexus-xr,session=<bench>; the C port
is devices/src/session.c) and run.sh --session no longer calls this (bare run.sh <elf> still does). Kept as the reference
the C resolver is checked against (core/qemu/tests/test_session_equiv.sh: same report, warnings,
errors, blob and manifest bytes on every bench); a change of rules goes into both.

The core manifest (docs/protocol.md `manifest`): what this QEMU core can emulate.

  manifest.py [--board <id>] [--board-config <unified config>] [--pretty] [--out file]

One JSON document, printed on one line (the control server sends the same bytes after `hello`),
built from the wiring model (scripts/wiring.py):
- boards: every emulated board (wiring.MACHINES: nexus-xr, speedybee-f405-wing), each with
  `machine` (its QEMU machine type), `config` (`unified`: a RotorFlight unified config and the
  custom-defaults blob; `cli`: a CLI script sent over the MSP UART) and `msp_uart` (the pad of the
  MSP/CLI host socket). `pads` = the board's pads (unified config: firmware id, kind, pins, timer,
  DMA, bus; boards/<id>/board.json: printed label, flags, onboard device with `detachable`; a CLI
  board's all from board.json), each with `devices` = what the machine models on it. `slots` = the
  old view the app's wiring wizard derives (app/scripts/WiringGraph.cs BoardInfo.Slots): gyroN,
  i2cN, uartN, motorN, servoN, in that order, kept unchanged for the current app (a pad of another
  group, e.g. an `output`, is not a slot). --board-config is the unified config of --board
  (default nexus-xr); the other boards use their own.
- devices: the device models' own descriptors (devices/include/fb/device.h) with their `ports`
  (the old single `kind` = the primary port's); `implemented` = a machine models one of its
  ports somewhere.
- aircraft: models/*/aircraft.json; `implemented` = the core has an FDM for it.
"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wiring  # noqa: E402

ROOT = wiring.ROOT


def manifest(board_config=None, board="nexus-xr"):
    boards = []
    for bid in wiring.MACHINES:
        b = wiring.load_board(bid, board_config if bid == board else None)
        boards.append({"id": bid, "name": b["name"], "mcu": b["mcu"], "machine": b["machine"],
                       "config": b["config"], "msp_uart": b["msp_uart"], "firmware_family": b["firmware_family"],
                       "slots": wiring.slots_of(b["pads"]),
                       "pads": [wiring.pad_public(p) for p in b["pads"]]})
    modelled = {d for spec in wiring.MACHINES.values() for d, _ in spec["modelled"]}
    devices = wiring.device_catalog()
    for d in devices:
        d["implemented"] = d["id"] in modelled
    aircraft = []
    for a in wiring.all_aircraft():
        a["implemented"] = wiring.has_fdm(a)
        aircraft.append(a)
    return {"type": "manifest", "core": "flightbench-qemu", "version": "0.1",
            "boards": boards, "devices": devices, "aircraft": aircraft}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="nexus-xr")
    ap.add_argument("--board-config")
    ap.add_argument("--pretty", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    m = manifest(a.board_config, a.board)
    text = json.dumps(m, indent=1 if a.pretty else None, separators=None if a.pretty else (",", ":"),
                      ensure_ascii=False) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()

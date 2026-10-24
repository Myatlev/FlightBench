#!/usr/bin/env python3
"""Build the 'custom defaults' blob a unified-target firmware expects in flash.

On real hardware the configurator inserts the board config text into the firmware image at the
address stored at 0x08002800 (two little-endian pointers: start, end). The ELF/hex alone has an
empty area, so an emulator must do the same insertion. This tool writes the blob; the core's machine
loads it at the start address (`defaults=`).

  mkdefaults.py <board.config> <out.bin> [--start 0x08002808] [--end 0x08004000] [--elf fw.elf]

With --elf the start/end pointers are read from the ELF's .custom_defaults section (8 bytes at
0x08002800) instead of the defaults. The text is NUL-terminated (customDefaultsHasNext stops at
0x00 or 0xFF). If the file does not begin with the firmware's expected prefix line it is prepended.
"""
import struct, sys

PREFIX = "## Rotorflight Custom Defaults"

def pointers_from_elf(path):
    data = open(path, "rb").read()
    # minimal ELF32 section walk: find a PT_LOAD segment covering 0x08002800 and read 8 bytes
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", data, off)
        if p_type == 1 and p_paddr <= 0x08002800 < p_paddr + p_filesz:
            o = p_offset + (0x08002800 - p_paddr)
            return struct.unpack_from("<II", data, o)
    raise SystemExit("custom defaults pointer (0x08002800) not found in ELF")

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    src, out = args[0], args[1]
    start, end = 0x08002808, 0x08004000
    if "--elf" in args:
        start, end = pointers_from_elf(args[args.index("--elf") + 1])
    if "--start" in args:
        start = int(args[args.index("--start") + 1], 0)
    if "--end" in args:
        end = int(args[args.index("--end") + 1], 0)
    text = open(src, encoding="utf-8").read().replace("\r\n", "\n")
    if not text.startswith(PREFIX):
        text = PREFIX + "\n" + text
    if not text.endswith("\n"):
        text += "\n"
    blob = text.encode("ascii") + b"\0"
    room = end - start
    if len(blob) > room:
        raise SystemExit(f"custom defaults too large: {len(blob)} > {room} bytes")
    open(out, "wb").write(blob)
    print(f"0x{start:08X}")  # the load address, consumed by the run script
    print(f"# {len(blob)} bytes, area 0x{start:08X}..0x{end:08X} ({room} bytes)", file=sys.stderr)

if __name__ == "__main__":
    main()

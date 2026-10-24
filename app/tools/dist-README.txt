FlightBench {{VERSION}} - Windows build ({{COMMIT}}, {{DATE}})
Fly the real flight-controller firmware binary on an emulated flight controller.

Quick start
  1. Run FlightBench.exe (nothing to install).
  2. Open "RS7 - First flight" from the Presets list.
  3. Watch the Status window until the firmware line reads "disarmed", then arm from the Transmitter window.
  Walkthrough with screenshots: https://github.com/Myatlev/flightbench/blob/main/docs/quickstart.md
  Source, documentation, issues:   https://github.com/Myatlev/flightbench

What is in this folder
  FlightBench.exe, data_FlightBench_windows_x86_64/   the desktop app (Godot 4 .NET)
  data/core/            the core: qemu-system-arm.exe (QEMU with FlightBench's STM32 machines) and its DLLs;
                        CORE-MANIFEST.txt lists the exact build
  data/boards/, data/models/, data/presets/            the bench data the app reads
  data/firmware/        the official RotorFlight 4.6.0 release hex; FIRMWARE.md gives its source and checksum

Licences
  FlightBench is free software under the GNU GPL, version 2 or (at your option) any later version - see LICENSE.
  The complete source for this exact build is the tag {{VERSION}} of https://github.com/Myatlev/flightbench.
  data/core/qemu-system-arm.exe is QEMU (GPL-2.0), built from the version named in CORE-MANIFEST.txt with the
  machine code and patches in core/qemu/ of the repository; QEMU's own source is at https://www.qemu.org.
  The DLLs next to it are MSYS2 builds of glib, gobject, gio, gmodule, pixman, zlib, zstd, bzip2, pcre2, libslirp,
  libffi, libiconv, gettext (libintl), ncurses and winpthreads, each under its own licence (LGPL, MIT, zlib, BSD);
  their sources are at https://packages.msys2.org.
  Godot Engine and the .NET runtime are MIT-licensed.
  The bundled RotorFlight firmware is GPL-3.0 software of the RotorFlight project; it is executed by the emulated
  MCU and never linked into FlightBench. See data/firmware/FIRMWARE.md.

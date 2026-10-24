FlightBench {{VERSION}} - macOS build ({{COMMIT}}, {{DATE}})
Fly the real flight-controller firmware binary on an emulated flight controller.

Quick start
  1. Open FlightBench.app (nothing to install; Apple silicon and Intel).
  2. Open "RS7 - First flight" from the Presets list.
  3. Watch the Status window until the firmware line reads "disarmed", then arm from the Transmitter window.
  Walkthrough with screenshots: https://github.com/Myatlev/flightbench/blob/main/docs/quickstart.md
  Source, documentation, issues:   https://github.com/Myatlev/flightbench

What is in FlightBench.app
  Contents/MacOS/FlightBench, Contents/Resources/data_FlightBench_macos_*/   the desktop app (Godot 4 .NET)
  Contents/Helpers/qemu-system-arm     the core: QEMU with FlightBench's STM32 machines; Contents/Frameworks/ holds
                                       its libraries; Contents/Resources/data/CORE-MANIFEST.txt lists the exact build
  Contents/Resources/data/boards, models, presets    the bench data the app reads
  Contents/Resources/data/firmware     the official RotorFlight 4.6.0 release hex; FIRMWARE.md gives its source and checksum

Licences
  FlightBench is free software under the GNU GPL, version 2 or (at your option) any later version - see LICENSE.
  The complete source for this exact build is the tag {{VERSION}} of https://github.com/Myatlev/flightbench.
  Contents/Helpers/qemu-system-arm is QEMU (GPL-2.0), built from the version named in CORE-MANIFEST.txt with the
  machine code and patches in core/qemu/ of the repository; QEMU's own source is at https://www.qemu.org.
  The dylibs in Contents/Frameworks are Homebrew builds of glib, gobject, gio, gmodule, pixman, libpng, zstd, pcre2,
  libslirp and gettext (libintl), each under its own licence (LGPL, MIT, zlib, BSD); their sources are at
  https://formulae.brew.sh.
  Godot Engine and the .NET runtime are MIT-licensed.
  The bundled RotorFlight firmware is GPL-3.0 software of the RotorFlight project; it is executed by the emulated
  MCU and never linked into FlightBench. See Contents/Resources/data/firmware/FIRMWARE.md.

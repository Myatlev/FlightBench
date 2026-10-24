# Cross-platform determinism: why the Windows core's hashes differ from Linux (2026-09-29)

> **2026-10-24:** the flight models were rewritten from first principles (`docs/fdm.md`); the flights and numbers
> below were made with the earlier models. The current baseline hashes are in `docs/contributing.md` (gates table).

Each platform is deterministic: two runs give byte-identical FDM logs. The two platforms give different logs
from the same machine code and firmware:

| bench (main `ab6d427`) | Linux (homelab, gcc 13.3, glibc) | Windows core (MSYS2 MINGW64, gcc 16.2) |
|---|---|---|
| trainer plane-launch, preset + sensors | `51d4a9212465` ×2 | `c66d93c7274e` ×2 |
| trainer plane-launch, preset only (`f4d83a5`) | `4f8c7114358a` | `ade0080623bb` |
| RS7 liftoff | `c5a2b9bb6c49` ×2 | `6d8d378c099f` ×2 |

A brief investigation, from the Linux side only. There is no Windows RS7 CSV in the repo, so the comparison
uses the trainer's plane-launch logs: `../sensors/linux-2026-09-29/` against `../sensors/windows-2026-09-29/`
(`plane-launch-2026-09-29-run1.csv`, 5400 rows each).

## Where the logs first differ
The first differing cell in each column (Linux vs Windows):

| t s | column | Linux | Windows | what it is |
|---|---|---|---|---|
| 4.26 | `fc_pitch` | −7.8 | −7.9 | iNav's attitude estimate converging on the pad (the model sits at 10°), one MSP decidegree, for 0.1 s; both −8.2 at 4.37 |
| 6.09 | `fc_yaw` | 352 | 353 | iNav's heading (integer degrees) |
| 7.47 | `fc_mode` | MANUAL | ACRO | iNav's gyro calibration ends 10 ms later on Linux (7.48 vs 7.47; the CAL blocker 7.58 vs 7.57) |
| 20.02 | `in3` | 1487 | 1489 | the first different firmware *output*: the elevator in ANGLE, still held on the pad |
| 24.17 | `motor_rpm`, `throttle` | 54, 0.069 | 0, 0 | the motor starts 10 ms earlier on Linux (launch detection) |
| 24.50.. | `ax`, `q`, `pitch`, `vz`, … | | | the physics follows the different commands (differences in the 3rd decimal) |

Every physics column (attitude, rates, speeds, positions, forces) is identical to the printed precision until
the firmware commands differ. The first start from an empty cfg-cache is identical too: the script timing,
acczero/accgain, **magzero −2 1 0, maggain 1497 1508 1499** (these are noise peaks of the compass model),
config applied at 72.889 s. So the split does not enter through the FDM's integration or the log's `printf`.
It is inside the firmware's estimator in the first 4 s after boot, i.e. in what the firmware reads (the
sensor registers) or when it reads them.

## libm is not the cause (tested on Linux)
The core calls `sin cos sincos log exp log2 pow atan2 asin hypot fmod lrint round sqrt` (`nm -D`). `sqrt` and
`lrint` are exact on every IEEE platform; the others may differ in the last bit between glibc and mingw-w64/UCRT.
To test whether a last-bit difference can reach the log, `nudge.c` (an `LD_PRELOAD` shim, below) moves the
result of chosen functions by 1 ulp for ~1 input in 16, and RS7 liftoff was flown with each group moved:

| moved by 1 ulp | RS7 FDM log (2 runs each) |
|---|---|
| none (the shim loaded) | `c5a2b9bb6c49` |
| `sin, cos, sincos` (IMU noise, attitude) | `c5a2b9bb6c49` |
| `log` (IMU / baro / GPS / compass noise) | `c5a2b9bb6c49` |
| `pow` (the baro's atmosphere) | `c5a2b9bb6c49` |
| `atan2, asin, hypot` | `c5a2b9bb6c49` |
| `exp, log2` | `c5a2b9bb6c49` |
| positive control: every call ×(1 + 1e−6) | `51d000c8a042`: first in `baro_reg_pa` 101325.25 → .35 at 0.68 s, in the flight at 30.8 s |

`libm_probe.c` shows why: the values reach the firmware as `float`, then as 16/24-bit sensor LSBs. With log, sin
and cos moved, the IMU's float inputs over 2^22 Box-Muller draws hash the same (`imu gyro (float)`,
`imu accel (float)` unchanged); a 1-ulp double error survives the float rounding about once in 2^29. The bench is
insensitive to libm rounding: it needs an error of ~10^9 ulp to change anything.

This rules out libm only for last-bit differences. A mingw-w64/UCRT function that is wrong by many ulp in some
range would still show up. `libm_probe.c` settles that on Windows in seconds (see below).

Other things checked and not the cause: the QEMU build has no FMA (`-O2 -msse2 -mcx16`, no `-march`; no `vfmadd`
in the binary), so there is no contraction to differ. The `lrint`/`long` paths (LLP64: `long` is 32-bit on
Windows) only see values far below 2^31.

## What is left, and how to find it
The difference is platform-dependent but not load-dependent: the Windows runs agree with each other, and so do
the Linux runs, under any host load. The candidates:
1. **Guest-visible timing that depends on the platform's QEMU main loop**: the virtual time at which an event
   lands (IMU sample vs SPI DMA read, UART/MSP bytes from the core's own poll, the first-access paths). On
   Windows, QEMU's main loop (win32 event handles, polling chardevs) and its set of `QEMU_CLOCK_REALTIME`
   timers differ, and `icount_get_limit()` caps each slice's budget at the next realtime deadline.
   `nexus_xr.c` already makes the idle skip independent of that (the comment at the skip). Any other place where
   a slice boundary or an `EXITTB` decides the virtual time the firmware sees would give exactly this
   picture: deterministic per platform, a small difference in the first seconds.
2. **A libm function that is badly wrong on Windows** (not last-bit): `libm_probe` shows it.
3. **Compiler differences** (gcc 16.2 vs 13.3): the FP code is IEEE-exact without `-ffast-math`/FMA, so only
   undefined behaviour (uninitialized reads, signed overflow) could differ. The least likely.

The next step (about an hour, both platforms):
- `cc -O2 -o libm_probe libm_probe.c -lm && ./libm_probe` on Windows (MINGW64), then diff against
  `libm_probe-linux.txt`. Equal lines rule out candidate 2.
- A short trainer boot (to t = 5 s) with `-M imu-log=<file>` on both platforms. The trace logs every IMU sample
  (`S`), INT1 fall (`F`) and SPI DMA read (`D`) with its virtual ns. The first differing line says whether the
  firmware reads the IMU at a different virtual time (candidate 1), or at the same time. If the time is the
  same, the values differ: add the sample's LSBs to the trace.

## Suggested fix (not implemented)
- **If it is timing (candidate 1):** find the event whose virtual time depends on the host loop and pin it to
  the virtual clock, as the idle skip already does: deliver host-side inputs (ctl/pilot/motion/MSP-poll bytes)
  at virtual deadlines that the core computes, never "when the main loop gets to it", and do not let realtime
  deadlines shape slices that end in guest-visible effects. That makes the hashes portable and keeps icount's
  determinism argument in one place.
- **If a libm function differs:** bring our own portable implementations of the few functions on the sensor
  paths (`sin`/`cos` for the Box-Muller and the attitude, `log`, `pow` for the atmosphere, `atan2`/`asin`/`exp`/
  `hypot` in the FDM), e.g. a correctly-rounded library (CORE-MATH) or the fdlibm-derived sources compiled into
  `devices/` and `fdm/`, so both platforms run the same code. Alternatively, make the noise generators
  libm-free: a polar method without `sin`/`cos`, or an integer Gaussian (sum of uniforms / a ziggurat table).
- Until then: hashes are per platform (`../plane/README.md`), and equivalence across platforms is checked on
  the flight numbers.

## Files
| file | what |
|---|---|
| `libm_probe.c` | libm fingerprint on the core's own inputs (the IMU noise's xorshift64* + Box-Muller, the baro's `pow` over 0..500 m, an attitude grid): one FNV-1a hash per function |
| `libm_probe-linux.txt` | its output on homelab (glibc 2.39, gcc 13.3) |
| `nudge.c` | the `LD_PRELOAD` shim of the sensitivity test (`FB_NUDGE=<functions>`, `FB_NUDGE_BIG=1` for the control); run QEMU through a wrapper that sets `LD_PRELOAD`, so Python's own libm is not touched |
| `csvdiff.py` | first differing cell per column between two FDM logs |
| `nudge-rs7-2026-09-29.log` | the RS7 runs of the table above |

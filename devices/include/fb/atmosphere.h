/* FlightBench: the standard atmosphere the barometer models are fed from.
 *
 * The same ICAO formulas the Nexus-XR machine uses for its DPS310 (core/qemu/hw/arm/nexus_xr.c,
 * baro_environment()): altitude h in metres above sea level (field elevation + FDM altitude),
 *   p = p0 * (1 - 2.25577e-5 h)^5.25588      p0 = sea-level pressure (101325 Pa by default)
 *   t = t0 - 0.0065 h                         t0 = sea-level temperature (degC)
 * h is clamped to 40 km. A machine that feeds a DPS310 and an SPL06 from these gets the same
 * pressure for the same FDM altitude on both chips.
 */
#ifndef FB_ATMOSPHERE_H
#define FB_ATMOSPHERE_H

#ifdef __cplusplus
extern "C" {
#endif

#define FB_ISA_P0_PA 101325.0

double fb_isa_pressure_pa(double p0_pa, double alt_m);
double fb_isa_temp_c(double t0_c, double alt_m);

#ifdef __cplusplus
}
#endif
#endif

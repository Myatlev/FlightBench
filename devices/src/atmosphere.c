/* Standard atmosphere for the barometer models. See include/fb/atmosphere.h. */
#include "fb/atmosphere.h"

#include <math.h>

static double clamp_h(double h) { return h > 40000.0 ? 40000.0 : h; }

double fb_isa_pressure_pa(double p0_pa, double alt_m)
{
    return p0_pa * pow(1.0 - 2.25577e-5 * clamp_h(alt_m), 5.25588);
}

double fb_isa_temp_c(double t0_c, double alt_m)
{
    return t0_c - 0.0065 * clamp_h(alt_m);
}

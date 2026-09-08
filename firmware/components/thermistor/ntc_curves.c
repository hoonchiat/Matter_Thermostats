/*
 * ntc_curves.c — pure NTC resistance<->temperature conversion (no hardware).
 * Uses the per-type LUTs with interpolation that is linear in ln(R) (a good fit
 * for NTC behavior over the tabulated 5 C steps), extrapolating past the ends.
 */
#include "thermistor.h"
#include <math.h>
#include <stddef.h>

#include "ntc_type2_lut.inc"
#include "ntc_type3_lut.inc"

static void select_lut(ntc_type_t type, const ntc_lut_point_t **tbl, int *len)
{
    switch (type) {
        case NTC_TYPE_2: *tbl = ntc_type2_lut; *len = ntc_type2_lut_len; break;
        case NTC_TYPE_3:
        default:         *tbl = ntc_type3_lut; *len = ntc_type3_lut_len; break;
    }
}

float ntc_resistance_from_mv(float v_mv, float vref_mv, float r_fix_ohm)
{
    /* Guard the poles: as V -> Vref the sensor looks open (R -> inf). */
    float denom = vref_mv - v_mv;
    if (denom < 1.0f)  denom = 1.0f;    /* avoid divide-by-zero / negative */
    if (v_mv   < 0.0f) v_mv  = 0.0f;
    return r_fix_ohm * (v_mv / denom);
}

float ntc_temp_c_from_resistance(ntc_type_t type, float r_ohm)
{
    const ntc_lut_point_t *t = NULL;
    int n = 0;
    select_lut(type, &t, &n);
    if (n < 2 || r_ohm <= 0.0f) {
        return -273.15f;                /* nonsensical input */
    }

    /* Table is temperature-ascending => resistance strictly descending. */
    const float lr = logf(r_ohm);

    /* Colder than the first point (resistance higher than t[0].r): extrapolate. */
    if (r_ohm >= (float)t[0].r_ohm) {
        float lr0 = logf((float)t[0].r_ohm);
        float lr1 = logf((float)t[1].r_ohm);
        float k   = (t[1].temp_c100 - t[0].temp_c100) / (lr1 - lr0);
        return (t[0].temp_c100 + k * (lr - lr0)) / 100.0f;
    }
    /* Hotter than the last point (resistance lower than t[n-1].r): extrapolate. */
    if (r_ohm <= (float)t[n - 1].r_ohm) {
        float lra = logf((float)t[n - 2].r_ohm);
        float lrb = logf((float)t[n - 1].r_ohm);
        float k   = (t[n - 1].temp_c100 - t[n - 2].temp_c100) / (lrb - lra);
        return (t[n - 1].temp_c100 + k * (lr - lrb)) / 100.0f;
    }

    /* In range: find the bracketing segment and interpolate in ln(R). */
    for (int i = 0; i < n - 1; ++i) {
        float ra = (float)t[i].r_ohm;
        float rb = (float)t[i + 1].r_ohm;
        if (r_ohm <= ra && r_ohm >= rb) {
            float lra = logf(ra);
            float lrb = logf(rb);
            float frac = (lr - lra) / (lrb - lra);   /* 0..1 across the segment */
            float c100 = t[i].temp_c100 +
                         frac * (t[i + 1].temp_c100 - t[i].temp_c100);
            return c100 / 100.0f;
        }
    }
    return (float)t[n / 2].temp_c100 / 100.0f;        /* unreachable fallback */
}

float ntc_temp_c_from_mv(ntc_type_t type, float v_mv, float vref_mv, float r_fix_ohm)
{
    return ntc_temp_c_from_resistance(type,
               ntc_resistance_from_mv(v_mv, vref_mv, r_fix_ohm));
}

/* Host unit tests for the pure NTC conversion (no ESP dependency). */
#include "thermistor.h"
#include <math.h>
#include <stdio.h>

static int fails = 0;
#define CHECK_NEAR(a, b, tol, msg) do { \
    double _a=(a), _b=(b); \
    if (fabs(_a-_b) > (tol)) { \
        printf("FAIL: %s: %.3f vs %.3f (tol %.3f)\n", msg, _a, _b, (double)(tol)); \
        fails++; \
    } else { printf("ok  : %s (%.3f)\n", msg, _a); } \
} while (0)

int main(void)
{
    /* Divider: Rfix=10k top, Vref=3300 mV, NTC bottom. At 25C Rntc=10k. */
    double r = ntc_resistance_from_mv(1650.0f, 3300.0f, 10000.0f);
    CHECK_NEAR(r, 10000.0, 5.0, "R from 1650mV == 10k");

    /* 10k at 25C -> ~25C on both curves. */
    CHECK_NEAR(ntc_temp_c_from_resistance(NTC_TYPE_2, 10000.0f), 25.0, 0.1, "type2 10k -> 25C");
    CHECK_NEAR(ntc_temp_c_from_resistance(NTC_TYPE_3, 10000.0f), 25.0, 0.1, "type3 10k -> 25C");

    /* Table anchor points (from the generated LUTs). */
    CHECK_NEAR(ntc_temp_c_from_resistance(NTC_TYPE_3, 12554.0f), 20.0, 0.2, "type3 12554 -> 20C");
    CHECK_NEAR(ntc_temp_c_from_resistance(NTC_TYPE_2, 12493.0f), 20.0, 0.2, "type2 12493 -> 20C");
    CHECK_NEAR(ntc_temp_c_from_resistance(NTC_TYPE_3,  8026.0f), 30.0, 0.2, "type3 8026 -> 30C");

    /* Interpolation between anchors stays monotonic & sensible (22.5C ~ mid). */
    double t = ntc_temp_c_from_resistance(NTC_TYPE_3, 11200.0f);
    CHECK_NEAR(t, 22.5, 1.0, "type3 11200 -> ~22.5C");

    /* End-to-end: node voltage -> temperature at room temp. */
    double te = ntc_temp_c_from_mv(NTC_TYPE_3, 1650.0f, 3300.0f, 10000.0f);
    CHECK_NEAR(te, 25.0, 0.2, "mv->temp at 1650mV -> 25C");

    /* Curves diverge away from 25C (Type 3 colder-reading than Type 2 at low R). */
    double t2 = ntc_temp_c_from_resistance(NTC_TYPE_2, 2538.0f); /* type2 60C anchor */
    double t3 = ntc_temp_c_from_resistance(NTC_TYPE_3, 2538.0f);
    if (fabs(t2 - t3) < 0.1) { printf("FAIL: curves should differ at 2538 ohm\n"); fails++; }
    else printf("ok  : curves differ at 2538 ohm (t2=%.2f t3=%.2f)\n", t2, t3);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL THERMISTOR TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

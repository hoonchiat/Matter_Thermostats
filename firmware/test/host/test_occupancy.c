/* Host unit tests for the pure occupancy vacancy-timeout logic. */
#include "occupancy.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

int main(void)
{
    const uint32_t timeout = 900;                 /* 15 min */

    /* Just saw motion -> occupied. */
    CHECK(occupancy_from_motion(1000, 1000, timeout), "motion now => occupied");

    /* 14 minutes since motion, timeout 15 -> still occupied. */
    CHECK(occupancy_from_motion(1000 + 14 * 60000ULL, 1000, timeout),
          "14 min < 15 min timeout => occupied");

    /* 16 minutes since motion -> unoccupied. */
    CHECK(!occupancy_from_motion(1000 + 16 * 60000ULL, 1000, timeout),
          "16 min > 15 min timeout => away");

    /* Exactly at the timeout boundary -> away (strict <). */
    CHECK(!occupancy_from_motion(0 + 900000ULL, 0, timeout),
          "exactly timeout => away");

    /* Clock anomaly (now < last_motion) -> fail safe to occupied. */
    CHECK(occupancy_from_motion(500, 1000, timeout), "clock anomaly => occupied");

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL OCCUPANCY TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

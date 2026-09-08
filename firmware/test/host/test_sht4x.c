/* Host unit tests for the pure SHT4x CRC + tick conversions. */
#include "sht4x.h"
#include <math.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

int main(void)
{
    /* Sensirion CRC-8 reference vector: {0xBE,0xEF} -> 0x92. */
    const uint8_t v[2] = { 0xBE, 0xEF };
    CHECK(sht4x_crc8(v, 2) == 0x92, "CRC-8 of BEEF == 0x92");

    /* Temperature endpoints and midpoint. */
    CHECK(NEAR(sht4x_ticks_to_c(0), -45.0, 0.001), "T ticks 0 -> -45C");
    CHECK(NEAR(sht4x_ticks_to_c(65535), 130.0, 0.001), "T ticks max -> 130C");
    CHECK(NEAR(sht4x_ticks_to_c(26214), 25.0, 0.05), "T ticks 26214 -> ~25C");

    /* Humidity endpoints (clamped) and midpoint. */
    CHECK(NEAR(sht4x_ticks_to_rh(0), 0.0, 0.001), "RH ticks 0 -> clamp 0%");
    CHECK(NEAR(sht4x_ticks_to_rh(65535), 100.0, 0.001), "RH ticks max -> clamp 100%");
    CHECK(NEAR(sht4x_ticks_to_rh(29360), 50.0, 0.1), "RH ticks 29360 -> ~50%");

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL SHT4X TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

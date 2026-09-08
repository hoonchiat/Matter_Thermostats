/*
 * test_ui.c — host render assertions for the OLED (no hardware).
 * Compiles ui_oled.c in UI_OLED_HOST mode and inspects the framebuffer, so a
 * regression that stops the humidity readout from rendering is caught in CI.
 */
#ifndef UI_OLED_HOST
#define UI_OLED_HOST
#endif
#include "../../components/ui_oled/ui_oled.c"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

/* Count lit pixels in a rectangle of the shared framebuffer. */
static int lit_pixels(int x0, int y0, int x1, int y1)
{
    int W = s.cfg.width, n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if ((s.fb[(y >> 3) * W + x] >> (y & 7)) & 1) n++;
    return n;
}

int main(void)
{
    ui_oled_config_t cfg = { .width = 128, .height = 64 };
    ui_oled_init(&cfg);

    ui_model_t m = {0};
    m.commissioned = true; m.occupied = true; m.screen = UI_SCREEN_HOME;
    m.mode = 1; m.temp_c100 = 2140; m.heat_set_c100 = 2000; m.cool_set_c100 = 2600;

    /* Bottom-right region where humidity is drawn. */
    const int X0 = 78, Y0 = 55, X1 = 127, Y1 = 63;

    /* SHT40 present: humidity valid -> the readout must render there. */
    m.humidity_valid = true; m.humidity_pct100 = 4500;   /* 45% */
    ui_oled_render(&m);
    CHECK(lit_pixels(X0, Y0, X1, Y1) > 0, "SHT40: humidity shown on HOME");

    /* NTC (no humidity): nothing drawn in the humidity region. */
    m.humidity_valid = false; m.humidity_pct100 = 0;
    ui_oled_render(&m);
    CHECK(lit_pixels(X0, Y0, X1, Y1) == 0, "NTC: no humidity on HOME");

    /* Running state (bottom-left) still renders regardless. */
    CHECK(lit_pixels(0, Y0, 50, Y1) > 0, "running-state text present");

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL UI TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

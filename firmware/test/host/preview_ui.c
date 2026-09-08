/*
 * preview_ui.c — render the OLED screens to the terminal as ASCII, on a host
 * PC, with no hardware. Compiles ui_oled.c in UI_OLED_HOST mode and dumps the
 * 1bpp framebuffer. Use it to eyeball the layout:  make preview
 */
#ifndef UI_OLED_HOST
#define UI_OLED_HOST
#endif
#include "../../components/ui_oled/ui_oled.c"

#include <stdio.h>

/* The framebuffer 's' is static inside ui_oled.c, which we #include above. */
static void dump(const char *title)
{
    int W = s.cfg.width, H = s.cfg.height;
    printf("\n%s\n+", title);
    for (int x = 0; x < W; ++x) putchar('-');
    printf("+\n");
    for (int y = 0; y < H; ++y) {
        putchar('|');
        for (int x = 0; x < W; ++x) {
            int bit = (s.fb[(y >> 3) * W + x] >> (y & 7)) & 1;
            putchar(bit ? '#' : ' ');
        }
        printf("|\n");
    }
    putchar('+');
    for (int x = 0; x < W; ++x) putchar('-');
    printf("+\n");
}

int main(void)
{
    ui_oled_config_t cfg = { .width = 128, .height = 64 };
    ui_oled_init(&cfg);

    ui_model_t m = {0};
    m.commissioned = true; m.occupied = true;
    m.temp_c100 = 2140; m.heat_set_c100 = 2000; m.cool_set_c100 = 2600;
    m.humidity_valid = true; m.humidity_pct100 = 4500;   /* 45% (SHT40) */

    /* HOME — heating call, fan HIGH, °C */
    m.screen = UI_SCREEN_HOME; m.mode = 1; m.calling_heat = true; m.fan_on = true;
    m.fan_speed = 3; m.active_setpoint = 0;
    ui_oled_render(&m); dump("HOME  (HEAT, calling, fan HIGH)");

    /* HOME — AWAY (occupancy setback), idle */
    m.calling_heat = false; m.fan_on = false; m.fan_speed = 0; m.occupied = false;
    ui_oled_render(&m); dump("HOME  (AWAY / occupancy setback)");
    m.occupied = true;

    /* HOME — AUTO, idle, fan AUTO, °F */
    m.mode = 3; m.calling_heat = false; m.calling_cool = false; m.fan_on = false;
    m.fan_speed = 0; m.fahrenheit = true;
    ui_oled_render(&m); dump("HOME  (AUTO, idle, fan AUTO, degF)");
    m.fahrenheit = false;

    /* ADJUST — heating setpoint */
    m.screen = UI_SCREEN_ADJUST; m.mode = 1; m.active_setpoint = 0;
    ui_oled_render(&m); dump("ADJUST (heat setpoint)");

    /* MENU — 7 rows; select PRESENCE (shows scrolling window + scrollbar) */
    m.screen = UI_SCREEN_MENU; m.menu_index = 1;
    const char *lines[] = { "FAN: AUTO", "PRESENCE: HOME", "OCC SRC: SENSOR",
                            "UNITS: C", "SENSOR: TYPE 3", "MATTER CODE >", "BACK" };
    for (int i = 0; i < 7; ++i) m.menu_lines[i] = lines[i];
    m.menu_count = 7;
    ui_oled_render(&m); dump("MENU  (PRESENCE selected; scrollbar)");

    /* INFO — Matter pairing code */
    m.screen = UI_SCREEN_INFO;
    m.info_title = "MATTER CODE"; m.info_line1 = "1234-567-8901"; m.info_line2 = "SCAN QR OR ENTER";
    ui_oled_render(&m); dump("INFO  (Matter pairing code)");

    /* PAIRING */
    m.screen = UI_SCREEN_HOME; m.commissioned = false; m.pairing_code = "1234-567-8901";
    ui_oled_render(&m); dump("PAIRING (uncommissioned)");

    /* FAULT */
    m.commissioned = true; m.fault = true;
    ui_oled_render(&m); dump("FAULT");

    return 0;
}

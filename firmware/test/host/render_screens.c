/*
 * render_screens.c — render every OLED screen with the real firmware renderer
 * (UI_OLED_HOST) and emit each 128x64 framebuffer as base64, keyed by id, as a
 * JSON object on stdout. Feeds tools/gen_userguide.py to build the user guide
 * with pixel-accurate device screens.
 */
#ifndef UI_OLED_HOST
#define UI_OLED_HOST
#endif
#include "../../components/ui_oled/ui_oled.c"

#include <stdio.h>
#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit_b64(const unsigned char *d, int n)
{
    for (int i = 0; i < n; i += 3) {
        int b0 = d[i];
        int b1 = (i + 1 < n) ? d[i + 1] : 0;
        int b2 = (i + 2 < n) ? d[i + 2] : 0;
        putchar(B64[b0 >> 2]);
        putchar(B64[((b0 & 3) << 4) | (b1 >> 4)]);
        putchar((i + 1 < n) ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        putchar((i + 2 < n) ? B64[b2 & 63] : '=');
    }
}

static int first = 1;
static void dump(const char *id)
{
    if (!first) printf(",\n");
    first = 0;
    printf("  \"%s\": \"", id);
    emit_b64(s.fb, s.cfg.width * s.cfg.height / 8);
    printf("\"");
}

int main(void)
{
    ui_oled_config_t cfg = { .width = 128, .height = 64 };
    ui_oled_init(&cfg);

    ui_model_t m = {0};
    m.commissioned = true; m.occupied = true;
    m.temp_c100 = 2140; m.heat_set_c100 = 2000; m.cool_set_c100 = 2600;

    printf("{\n");

    /* HOME — NTC build (no humidity), HEAT calling, fan HIGH, °C */
    m.screen = UI_SCREEN_HOME; m.mode = 1; m.calling_heat = true; m.fan_on = true;
    m.fan_speed = 3; m.humidity_valid = false;
    ui_oled_render(&m); dump("home_heat");

    /* HOME — SHT40 build, AUTO idle, fan AUTO, °C, humidity 45% */
    m.mode = 3; m.calling_heat = false; m.fan_on = false; m.fan_speed = 0;
    m.humidity_valid = true; m.humidity_pct100 = 4500;
    ui_oled_render(&m); dump("home_sht");

    /* HOME — COOL calling, °F, humidity */
    m.mode = 2; m.calling_cool = true; m.fan_on = true; m.fan_speed = 2; m.fahrenheit = true;
    ui_oled_render(&m); dump("home_f");
    m.fahrenheit = false; m.calling_cool = false; m.fan_on = false; m.fan_speed = 0;

    /* HOME — AWAY (occupancy setpoints in effect), humidity */
    m.mode = 1; m.occupied = false;
    ui_oled_render(&m); dump("home_away");
    m.occupied = true;

    /* ADJUST — heating setpoint */
    m.screen = UI_SCREEN_ADJUST; m.mode = 1; m.active_setpoint = 0;
    ui_oled_render(&m); dump("adjust");

    /* MENU — top (FAN selected) */
    {
        static const char *lines[] = { "FAN: AUTO", "PRESENCE: HOME", "OCC SRC: SENSOR",
                                       "UNITS: C", "SENSOR: SHT40", "MATTER CODE >", "BACK" };
        for (int i = 0; i < 7; ++i) m.menu_lines[i] = lines[i];
        m.menu_count = 7;
        m.screen = UI_SCREEN_MENU; m.menu_index = 0;
        ui_oled_render(&m); dump("menu_top");
        m.menu_index = 4;   /* scrolled: SENSOR row selected */
        ui_oled_render(&m); dump("menu_scroll");
    }

    /* INFO — Matter pairing code */
    m.screen = UI_SCREEN_INFO;
    m.info_title = "MATTER CODE"; m.info_line1 = "1234-567-8901"; m.info_line2 = "SCAN QR OR ENTER";
    ui_oled_render(&m); dump("info");

    /* PAIRING — uncommissioned */
    m.screen = UI_SCREEN_HOME; m.commissioned = false; m.pairing_code = "1234-567-8901";
    ui_oled_render(&m); dump("pairing");

    /* FAULT — sensor fault */
    m.commissioned = true; m.fault = true;
    ui_oled_render(&m); dump("fault");

    printf("\n}\n");
    return 0;
}

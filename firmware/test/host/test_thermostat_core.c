/* Host unit tests for the pure control law (no ESP dependency). */
#include "thermostat_core.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

static thermo_input_t base(void)
{
    thermo_input_t in = {0};
    in.mode = THERMO_MODE_HEAT;
    in.heat_set_c = 20.0f;
    in.cool_set_c = 26.0f;
    in.temp_c = 21.0f;
    in.now_ms = 0;
    return in;
}

int main(void)
{
    thermo_config_t cfg = thermo_core_default_config();  /* deadband 1.0, min_off 300, etc. */

    /* --- OFF / fault fail-safe --- */
    {
        thermo_state_t st; thermo_core_init(&st, 0);
        thermo_input_t in = base(); in.mode = THERMO_MODE_OFF; in.temp_c = 0.0f;
        thermo_output_t o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.w_heat && !o.y_cool && !o.g_fan, "OFF => all outputs off");

        in.mode = THERMO_MODE_HEAT; in.fault = true;
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.w_heat && !o.y_cool && !o.g_fan, "fault => all outputs off");
    }

    /* --- Heating hysteresis (non-compressor: immediate) --- */
    {
        thermo_state_t st; thermo_core_init(&st, 0);
        thermo_input_t in = base();
        in.temp_c = 19.4f;                 /* below 20 - 0.5 => call heat */
        thermo_output_t o = thermo_core_step(&cfg, &in, &st);
        CHECK(o.w_heat, "heat on below setpoint-halfband");
        CHECK(o.g_fan,  "fan follows heat call");

        in.temp_c = 20.2f;                 /* inside band => hold on */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(o.w_heat, "heat holds within band");

        in.temp_c = 20.6f;                 /* above 20 + 0.5 => stop */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.w_heat, "heat off above setpoint+halfband");
    }

    /* --- Cooling min-off (anti short-cycle) --- */
    {
        thermo_state_t st; thermo_core_init(&st, 0);
        thermo_input_t in = base();
        in.mode = THERMO_MODE_COOL;
        in.now_ms = 40000;                 /* past startup lockout (30 s) */
        in.temp_c = 27.0f;                 /* above 26 + 0.5 => want cool */
        thermo_output_t o = thermo_core_step(&cfg, &in, &st);
        CHECK(o.y_cool, "cool on when hot and lockout elapsed");

        in.temp_c = 25.0f;                 /* satisfied => turn off (min_on 120s?) */
        in.now_ms = 40000 + 130000;        /* +130 s so min_on satisfied */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.y_cool, "cool off after satisfied and min_on met");

        in.temp_c = 27.0f;                 /* hot again immediately */
        in.now_ms += 1000;                 /* only 1 s after off => min_off blocks */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.y_cool, "cool blocked by min_off right after turning off");
        CHECK(o.call_delayed, "call_delayed set while gated by min_off");

        in.now_ms += 300000;               /* +300 s => min_off satisfied */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(o.y_cool, "cool resumes after min_off elapses");
    }

    /* --- Auto: never both, dead-zone respected --- */
    {
        thermo_state_t st; thermo_core_init(&st, 0);
        thermo_input_t in = base();
        in.mode = THERMO_MODE_AUTO;
        in.heat_set_c = 20.0f; in.cool_set_c = 24.0f;
        in.now_ms = 40000;
        in.temp_c = 22.0f;                 /* in the dead-zone */
        thermo_output_t o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.w_heat && !o.y_cool, "auto idle inside dead-zone");

        in.temp_c = 18.0f;                 /* cold => heat only */
        o = thermo_core_step(&cfg, &in, &st);
        CHECK(o.w_heat && !o.y_cool, "auto heats when cold, not cool");
    }

    /* --- Startup lockout blocks the first compressor call --- */
    {
        thermo_state_t st; thermo_core_init(&st, 0);
        thermo_input_t in = base();
        in.mode = THERMO_MODE_COOL;
        in.now_ms = 5000;                  /* within 30 s lockout */
        in.temp_c = 30.0f;
        thermo_output_t o = thermo_core_step(&cfg, &in, &st);
        CHECK(!o.y_cool, "startup lockout blocks early compressor call");
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL CONTROL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

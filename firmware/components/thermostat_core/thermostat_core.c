#include "thermostat_core.h"

/* ---- small helpers -------------------------------------------------------- */

static inline uint64_t secs_to_ms(uint32_t s) { return (uint64_t)s * 1000ULL; }

/* Heating hysteresis: turn on below (set - h), off above (set + h), else hold. */
static bool heat_hysteresis(float temp, float set, float h, bool currently_on)
{
    if (currently_on) {
        return temp < (set + h);       /* stay on until we reach set + h */
    }
    return temp <= (set - h);          /* turn on once we fall to set - h */
}

/* Cooling hysteresis: turn on above (set + h), off below (set - h), else hold. */
static bool cool_hysteresis(float temp, float set, float h, bool currently_on)
{
    if (currently_on) {
        return temp > (set - h);       /* stay on until we reach set - h */
    }
    return temp >= (set + h);          /* turn on once we rise to set + h */
}

/* May we turn a (possibly compressor) circuit ON right now?
 * min_off only applies once the circuit has actually cycled off before; the
 * post-boot period is governed by startup_lockout, not by a phantom off event. */
static bool allow_turn_on(bool is_compressor, uint64_t now, uint64_t last_off,
                          bool off_valid, uint64_t boot,
                          const thermo_config_t *cfg, bool *delayed)
{
    if (!is_compressor) {
        return true;                   /* e.g. gas furnace: respond immediately */
    }
    if (now - boot < secs_to_ms(cfg->startup_lockout_s)) {
        if (delayed) *delayed = true;  /* still in post-boot lockout */
        return false;
    }
    if (off_valid && (now - last_off) < secs_to_ms(cfg->min_off_s)) {
        if (delayed) *delayed = true;  /* anti-short-cycle: min OFF not met */
        return false;
    }
    return true;
}

/* May we turn a circuit OFF right now (min-on satisfied)? Safety-off overrides. */
static bool allow_turn_off(bool is_compressor, uint64_t now, uint64_t last_on,
                           const thermo_config_t *cfg)
{
    if (!is_compressor) {
        return true;
    }
    return (now - last_on) >= secs_to_ms(cfg->min_on_s);
}

/* ---- public API ----------------------------------------------------------- */

void thermo_core_init(thermo_state_t *st, uint64_t now_ms)
{
    st->heating = false;
    st->cooling = false;
    st->heat_on_ms  = now_ms;
    st->heat_off_ms = now_ms;
    st->cool_on_ms  = now_ms;
    st->cool_off_ms = now_ms;
    st->heat_off_valid = false;
    st->cool_off_valid = false;
    st->boot_ms     = now_ms;
    st->initialized = true;
}

thermo_config_t thermo_core_default_config(void)
{
    thermo_config_t c = {
        .deadband_c          = 1.0f,
        .auto_min_deadzone_c = 2.0f,
        .min_off_s           = 300,
        .min_on_s            = 120,
        .startup_lockout_s   = 30,
        .hp_mode             = THERMO_HP_NONE,
        .fan_call_speed      = THERMO_FAN_HIGH,
    };
    return c;
}

thermo_output_t thermo_core_step(const thermo_config_t *cfg,
                                 const thermo_input_t  *in,
                                 thermo_state_t        *st)
{
    thermo_output_t out = {0};         /* default: EVERYTHING OFF (fail-safe) */

    if (!st->initialized) {
        thermo_core_init(st, in->now_ms);
    }

    /* A fixed (non-auto) fan speed means continuous circulation at that level. */
    const int fixed_fan = (in->fan_speed >= THERMO_FAN_LOW) ? in->fan_speed : 0;
    int call_speed = cfg->fan_call_speed;
    if (call_speed < THERMO_FAN_LOW || call_speed > THERMO_FAN_HIGH) call_speed = THERMO_FAN_HIGH;

    /* Fault: force all outputs off, remember the off transition, bail. */
    if (in->fault) {
        if (st->heating) { st->heating = false; st->heat_off_ms = in->now_ms; st->heat_off_valid = true; }
        if (st->cooling) { st->cooling = false; st->cool_off_ms = in->now_ms; st->cool_off_valid = true; }
        return out;                    /* fan off too under fault */
    }

    /* Off / Fan-only: no heat/cool. Fan-only runs continuously (at the chosen
     * speed, or the call speed if AUTO); Off still honors a fixed circulation
     * speed but idles when the fan is AUTO. */
    if (in->mode == THERMO_MODE_OFF || in->mode == THERMO_MODE_FAN_ONLY) {
        if (st->heating) { st->heating = false; st->heat_off_ms = in->now_ms; st->heat_off_valid = true; }
        if (st->cooling) { st->cooling = false; st->cool_off_ms = in->now_ms; st->cool_off_valid = true; }
        if (in->mode == THERMO_MODE_FAN_ONLY) {
            out.fan_level = fixed_fan > 0 ? fixed_fan : call_speed;
        } else {
            out.fan_level = fixed_fan;      /* OFF: only a fixed speed circulates */
        }
        out.g_fan = out.fan_level > 0;
        return out;
    }

    const float h = cfg->deadband_c * 0.5f;

    /* In AUTO, guarantee a minimum gap between the two setpoints. */
    float cool_set = in->cool_set_c;
    if (in->mode == THERMO_MODE_AUTO) {
        float min_cool = in->heat_set_c + cfg->auto_min_deadzone_c;
        if (cool_set < min_cool) cool_set = min_cool;
    }

    bool want_heat = false, want_cool = false;
    if (in->mode == THERMO_MODE_HEAT || in->mode == THERMO_MODE_AUTO) {
        want_heat = heat_hysteresis(in->temp_c, in->heat_set_c, h, st->heating);
    }
    if (in->mode == THERMO_MODE_COOL || in->mode == THERMO_MODE_AUTO) {
        want_cool = cool_hysteresis(in->temp_c, cool_set, h, st->cooling);
    }
    /* Never both (should not happen once the AUTO dead-zone is enforced). */
    if (want_heat && want_cool) { want_heat = false; want_cool = false; }

    const bool heat_is_compressor = (cfg->hp_mode != THERMO_HP_NONE);
    const bool cool_is_compressor = true;

    /* --- Heat channel ------------------------------------------------------ */
    if (st->heating && !want_heat) {
        if (allow_turn_off(heat_is_compressor, in->now_ms, st->heat_on_ms, cfg)) {
            st->heating = false;
            st->heat_off_ms = in->now_ms;
            st->heat_off_valid = true;
        }
        /* else: min-on not met — hold on a bit longer */
    } else if (!st->heating && want_heat && !st->cooling) {
        if (allow_turn_on(heat_is_compressor, in->now_ms, st->heat_off_ms,
                          st->heat_off_valid, st->boot_ms, cfg, &out.call_delayed)) {
            st->heating = true;
            st->heat_on_ms = in->now_ms;
        }
    }

    /* --- Cool channel (mutually exclusive with heat) ----------------------- */
    if (st->cooling && !want_cool) {
        if (allow_turn_off(cool_is_compressor, in->now_ms, st->cool_on_ms, cfg)) {
            st->cooling = false;
            st->cool_off_ms = in->now_ms;
            st->cool_off_valid = true;
        }
    } else if (!st->cooling && want_cool && !st->heating) {
        if (allow_turn_on(cool_is_compressor, in->now_ms, st->cool_off_ms,
                          st->cool_off_valid, st->boot_ms, cfg, &out.call_delayed)) {
            st->cooling = true;
            st->cool_on_ms = in->now_ms;
        }
    }

    /* --- Map internal state to outputs ------------------------------------- */
    out.w_heat = st->heating;
    out.y_cool = st->cooling;

    /* Fan: run at the call speed while heating/cooling; a fixed circulation
     * speed applies whenever it is higher (or when idle). */
    int fan_level = (st->heating || st->cooling) ? call_speed : 0;
    if (fixed_fan > fan_level) fan_level = fixed_fan;
    out.fan_level = fan_level;
    out.g_fan  = fan_level > 0;

    switch (cfg->hp_mode) {
        case THERMO_HP_O_IN_COOL: out.ob_reversing = st->cooling; break;
        case THERMO_HP_B_IN_HEAT: out.ob_reversing = st->heating; break;
        case THERMO_HP_NONE:
        default:                  out.ob_reversing = false;       break;
    }

    return out;
}

/*
 * thermostat_core — pure HVAC control law (hysteresis + cycle protection).
 *
 * No I/O, no RTOS, no ESP dependencies: it takes a snapshot of inputs and the
 * previous state and returns the desired output state. This makes the
 * safety-relevant decision logic unit-testable on a host PC.
 *
 * Temperatures are in whole degrees Celsius as floats here for readability at
 * the control-law layer; the Matter glue converts to/from 0.01 C int16.
 */
#ifndef THERMOSTAT_CORE_H
#define THERMOSTAT_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THERMO_MODE_OFF = 0,
    THERMO_MODE_HEAT,
    THERMO_MODE_COOL,
    THERMO_MODE_AUTO,
    THERMO_MODE_FAN_ONLY,
} thermo_mode_t;

/* Reversing-valve behavior for heat-pump systems. */
typedef enum {
    THERMO_HP_NONE = 0,     /* conventional system, no O/B output           */
    THERMO_HP_O_IN_COOL,    /* energize O/B during a cool call (typical)    */
    THERMO_HP_B_IN_HEAT,    /* energize O/B during a heat call              */
} thermo_hp_mode_t;

/* Static configuration (from NVS / Kconfig). Units as noted. */
typedef struct {
    float    deadband_c;         /* total hysteresis band, e.g. 1.0          */
    float    auto_min_deadzone_c;/* min gap between heat & cool in AUTO       */
    uint32_t min_off_s;          /* compressor anti-short-cycle OFF time     */
    uint32_t min_on_s;           /* minimum ON time once energized           */
    uint32_t startup_lockout_s;  /* lockout after boot before any comp. call */
    thermo_hp_mode_t hp_mode;    /* reversing valve mapping                  */
} thermo_config_t;

/* Live inputs for one control step. */
typedef struct {
    thermo_mode_t mode;
    float         temp_c;        /* measured room temperature                */
    float         heat_set_c;    /* heating setpoint                         */
    float         cool_set_c;    /* cooling setpoint                         */
    bool          fault;         /* sensor fault → force everything off      */
    bool          fan_request;   /* external (Matter) continuous-fan request */
    uint64_t      now_ms;        /* monotonic time                          */
} thermo_input_t;

/* Persisted internal state across steps (owned by the caller). */
typedef struct {
    bool     heating;            /* W currently energized                    */
    bool     cooling;            /* Y currently energized                    */
    uint64_t heat_on_ms;         /* when heating last turned on              */
    uint64_t heat_off_ms;        /* when heating last turned off             */
    uint64_t cool_on_ms;         /* when cooling last turned on              */
    uint64_t cool_off_ms;        /* when cooling last turned off             */
    bool     heat_off_valid;     /* a real heat off-transition has occurred  */
    bool     cool_off_valid;     /* a real cool off-transition has occurred  */
    uint64_t boot_ms;            /* time reference for startup lockout       */
    bool     initialized;
} thermo_state_t;

/* Desired outputs after one step. */
typedef struct {
    bool w_heat;
    bool y_cool;
    bool g_fan;
    bool ob_reversing;
    bool call_delayed;           /* a call is wanted but gated by a timer     */
} thermo_output_t;

/* Initialize state (call once, passing the current monotonic time). */
void thermo_core_init(thermo_state_t *st, uint64_t now_ms);

/* Sensible default configuration. */
thermo_config_t thermo_core_default_config(void);

/*
 * Run one control step. Returns the desired outputs and updates *st.
 * Guarantees:
 *   - fault or MODE_OFF  => all outputs off immediately (fail-safe).
 *   - never energizes heat and cool simultaneously.
 *   - honors min_off / min_on / startup_lockout for compressor circuits.
 */
thermo_output_t thermo_core_step(const thermo_config_t *cfg,
                                 const thermo_input_t  *in,
                                 thermo_state_t        *st);

#ifdef __cplusplus
}
#endif

#endif /* THERMOSTAT_CORE_H */

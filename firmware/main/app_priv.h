/*
 * app_priv.h — shared application state, events, and cross-module declarations.
 * Included by app_main / app_matter / app_control / app_nvs.
 */
#ifndef APP_PRIV_H
#define APP_PRIV_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "thermostat_core.h"
#include "thermistor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- persisted user configuration (NVS namespace "thermo_cfg") ------------ */
typedef struct {
    int  mode;              /* thermo_mode_t                     */
    int  heat_set_c100;     /* 0.01 C                            */
    int  cool_set_c100;     /* 0.01 C                            */
    int  deadband_c10;      /* 0.1 C units                       */
    int  offset_c100;       /* calibration, 0.01 C               */
    bool fahrenheit;        /* display units                     */
    int  ntc_type;          /* ntc_type_t                        */
    int  min_off_s;
    int  min_on_s;
    int  hp_reversing;      /* thermo_hp_mode_t                  */
    int  brightness;        /* 0..255                            */
    int  fan_speed;         /* thermo_fan_speed_t: 0=auto,1=low,2=med,3=high */
    int  occ_source;        /* occ_source_t: 0=manual, 1=sensor  */
    bool occ_manual_home;   /* manual Home(true)/Away(false) pref */
    int  away_heat_c10;     /* Away heating setback (0.1 C)       */
    int  away_cool_c10;     /* Away cooling setback (0.1 C)       */
} app_config_t;

/* Occupancy source. */
typedef enum { OCC_SRC_MANUAL = 0, OCC_SRC_SENSOR = 1 } occ_source_t;

/* ---- live application state (single source of truth) ---------------------- */
typedef struct {
    app_config_t cfg;

    int  temp_c100;         /* measured room temp, 0.01 C        */
    bool fault;             /* sensor fault                      */

    bool calling_heat;
    bool calling_cool;
    bool fan_on;
    bool occupied;          /* resolved live Home(true)/Away(false) */

    bool commissioned;      /* Matter commissioned onto a fabric */
    int  thread_rssi;

    int  screen;            /* ui_screen_t                       */
    int  active_setpoint;   /* 0 = heat, 1 = cool                */
    int  menu_index;        /* highlighted settings-menu row     */
} app_state_t;

/* ---- inter-task events ---------------------------------------------------- */
typedef enum {
    EVT_ENCODER_DELTA = 0,  /* value = signed detents            */
    EVT_BTN_MODE_SHORT,     /* dedicated push button, short      */
    EVT_BTN_MENU_LONG,      /* dedicated push button, long       */
    EVT_ENC_SW_SHORT,       /* encoder switch, short             */
    EVT_ENC_SW_LONG,        /* encoder switch, long              */
    EVT_RESET_LONG,         /* BOOT button long → factory reset  */
    EVT_MATTER_SET_MODE,    /* value = thermo_mode_t             */
    EVT_MATTER_SET_HEAT,    /* value = 0.01 C                    */
    EVT_MATTER_SET_COOL,    /* value = 0.01 C                    */
    EVT_MATTER_SET_UNITS,   /* value = 0/1 (C/F)                 */
    EVT_MATTER_SET_FAN,     /* value = thermo_fan_speed_t (0..3) */
    EVT_MATTER_COMMISSIONED,/* value = 0/1                       */
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    int32_t          value;
} app_event_t;

/* ---- globals -------------------------------------------------------------- */
extern app_state_t     g_state;
extern QueueHandle_t   g_event_q;   /* app_event_t */
extern SemaphoreHandle_t g_state_mtx;

static inline void app_lock(void)   { xSemaphoreTake(g_state_mtx, portMAX_DELAY); }
static inline void app_unlock(void) { xSemaphoreGive(g_state_mtx); }

/* Post an event from any context (drivers, Matter callbacks). */
void app_post_event(app_event_type_t type, int32_t value);

/* ---- module entry points -------------------------------------------------- */
void app_config_defaults(app_config_t *cfg);
int  app_nvs_load(app_config_t *cfg);      /* fills defaults if missing */
int  app_nvs_save(const app_config_t *cfg);/* debounced writer inside   */

int  app_matter_start(void);               /* create endpoints + start stack */
void app_matter_report_temperature(int temp_c100, bool fault);
void app_matter_report_running_state(bool heat, bool cool, bool fan);
void app_matter_report_setpoints(int heat_c100, int cool_c100);
void app_matter_report_mode(int mode);
void app_matter_report_units(bool fahrenheit);   /* TemperatureDisplayMode */
void app_matter_report_fan(int fan_speed);       /* Fan Control FanMode */
void app_matter_report_occupancy(bool occupied); /* Occupancy Sensing */
void app_matter_factory_reset(void);
void app_matter_get_pairing_code(char *out, int out_len);

void app_control_start(void);              /* sensor + control + ui tasks */

#ifdef __cplusplus
}
#endif

#endif /* APP_PRIV_H */

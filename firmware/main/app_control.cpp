/*
 * app_control.cpp — the runtime: sensor task, control task, and UI task.
 * Ties together the thermistor, thermostat_core, relays, rotary_encoder,
 * button and ui_oled components, and keeps g_state and Matter in sync.
 */
#include "app_priv.h"

#include <stdlib.h>
#include <limits.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ui_oled.h"
#include "rotary_encoder.h"
#include "button.h"
#include "relays.h"

#define TAG "app_control"

/* Setpoint hard limits (0.01 C), matching the Matter Abs*SetpointLimit attrs. */
#define HEAT_MIN 700
#define HEAT_MAX 3000
#define COOL_MIN 1600
#define COOL_MAX 3200

/* Button identifiers. */
enum { BTN_ID_PUSH = 1, BTN_ID_ENC_SW = 2, BTN_ID_RESET = 3 };

static QueueHandle_t s_btn_q;      /* button_event_t from the button component */
static int64_t s_last_adjust_ms;   /* for ADJUST auto-timeout                  */

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

/* Step per detent in 0.01 C: 0.5 C, or ~0.5 F when displaying Fahrenheit. */
static int setpoint_step_c100(void)
{
    app_lock();
    bool f = g_state.cfg.fahrenheit;
    app_unlock();
    return f ? 28 : 50;   /* 0.5 F ≈ 0.28 C -> 28; 0.5 C -> 50 */
}

/* ---- sensor task ---------------------------------------------------------- */

static void sensor_task(void *arg)
{
    (void)arg;
    thermistor_config_t tc = {
        .adc_gpio      = CONFIG_THERMO_PIN_NTC_ADC,
        .r_fix_ohm     = CONFIG_THERMO_DIVIDER_RFIX_OHM,
        .vref_mv       = CONFIG_THERMO_DIVIDER_VREF_MV,
        .type          = (ntc_type_t)g_state.cfg.ntc_type,
        .median_n      = CONFIG_THERMO_ADC_MEDIAN_N,
        .ema_alpha_pct = CONFIG_THERMO_EMA_ALPHA_PCT,
        .offset_c100   = g_state.cfg.offset_c100,
    };
    if (thermistor_init(&tc) != ESP_OK) {
        ESP_LOGE(TAG, "thermistor init failed");
    }

    int last_reported = INT32_MIN;
    int64_t last_report_ms = 0;

    for (;;) {
        float temp_c = 0.0f;
        bool fault = false;
        thermistor_read(&temp_c, &fault);
        int temp_c100 = (int)(temp_c * 100.0f + (temp_c >= 0 ? 0.5f : -0.5f));

        app_lock();
        g_state.temp_c100 = temp_c100;
        g_state.fault = fault;
        app_unlock();

        /* Rate-limited Matter report: on >=0.1C change or every 30 s. */
        int64_t t = now_ms();
        if (fault) {
            app_matter_report_temperature(0, true);
            last_reported = INT32_MIN;
        } else if (abs(temp_c100 - last_reported) >= 10 || (t - last_report_ms) > 30000) {
            app_matter_report_temperature(temp_c100, false);
            last_reported = temp_c100;
            last_report_ms = t;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- control task --------------------------------------------------------- */

static void control_task(void *arg)
{
    (void)arg;
    relays_config_t rc = {
        .gpio_w  = CONFIG_THERMO_PIN_RELAY_W,
        .gpio_y  = CONFIG_THERMO_PIN_RELAY_Y,
        .gpio_g  = CONFIG_THERMO_PIN_RELAY_G,
        .gpio_ob = CONFIG_THERMO_PIN_RELAY_OB,
        .active_high = true,
    };
    relays_init(&rc);

    thermo_state_t core = {0};
    thermo_core_init(&core, now_ms());

    bool last_h = false, last_c = false, last_f = false;

    for (;;) {
        app_lock();
        app_config_t cfg = g_state.cfg;
        int temp_c100 = g_state.temp_c100;
        bool fault = g_state.fault;
        app_unlock();

        thermo_config_t tcfg = thermo_core_default_config();
        tcfg.deadband_c = cfg.deadband_c10 / 10.0f;
        tcfg.auto_min_deadzone_c = CONFIG_THERMO_AUTO_MIN_DEADZONE_C10 / 10.0f;
        tcfg.min_off_s = cfg.min_off_s;
        tcfg.min_on_s  = cfg.min_on_s;
        tcfg.startup_lockout_s = CONFIG_THERMO_STARTUP_LOCKOUT_S;
        tcfg.hp_mode   = (thermo_hp_mode_t)cfg.hp_reversing;

        thermo_input_t in = {
            .mode        = (thermo_mode_t)cfg.mode,
            .temp_c      = temp_c100 / 100.0f,
            .heat_set_c  = cfg.heat_set_c100 / 100.0f,
            .cool_set_c  = cfg.cool_set_c100 / 100.0f,
            .fault       = fault,
            .fan_request = false,
            .now_ms      = (uint64_t)now_ms(),
        };
        thermo_output_t out = thermo_core_step(&tcfg, &in, &core);

        relays_state_t rs = { .w = out.w_heat, .y = out.y_cool,
                              .g = out.g_fan, .ob = out.ob_reversing };
        relays_apply(&rs);

        app_lock();
        g_state.calling_heat = out.w_heat;
        g_state.calling_cool = out.y_cool;
        g_state.fan_on = out.g_fan;
        app_unlock();

        if (out.w_heat != last_h || out.y_cool != last_c || out.g_fan != last_f) {
            app_matter_report_running_state(out.w_heat, out.y_cool, out.g_fan);
            last_h = out.w_heat; last_c = out.y_cool; last_f = out.g_fan;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---- button translation --------------------------------------------------- */

static void button_task(void *arg)
{
    (void)arg;
    button_event_t ev;
    for (;;) {
        if (xQueueReceive(s_btn_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        switch (ev.id) {
            case BTN_ID_PUSH:
                app_post_event(ev.type == BUTTON_EVENT_LONG ? EVT_BTN_MENU_LONG
                                                            : EVT_BTN_MODE_SHORT, 0);
                break;
            case BTN_ID_ENC_SW:
                app_post_event(ev.type == BUTTON_EVENT_LONG ? EVT_ENC_SW_LONG
                                                            : EVT_ENC_SW_SHORT, 0);
                break;
            case BTN_ID_RESET:
                if (ev.type == BUTTON_EVENT_LONG) app_post_event(EVT_RESET_LONG, 0);
                break;
        }
    }
}

/* ---- event handling (mutates g_state) ------------------------------------- */

static void cycle_mode(void)
{
    /* OFF -> HEAT -> COOL -> AUTO -> OFF */
    int m = g_state.cfg.mode;
    switch (m) {
        case THERMO_MODE_OFF:  m = THERMO_MODE_HEAT; break;
        case THERMO_MODE_HEAT: m = THERMO_MODE_COOL; break;
        case THERMO_MODE_COOL: m = THERMO_MODE_AUTO; break;
        default:               m = THERMO_MODE_OFF;  break;
    }
    g_state.cfg.mode = m;
}

static void apply_encoder(int delta)
{
    int step = setpoint_step_c100() * delta;
    if (g_state.active_setpoint == 1) {
        g_state.cfg.cool_set_c100 = clampi(g_state.cfg.cool_set_c100 + step, COOL_MIN, COOL_MAX);
    } else {
        g_state.cfg.heat_set_c100 = clampi(g_state.cfg.heat_set_c100 + step, HEAT_MIN, HEAT_MAX);
    }
    g_state.screen = UI_SCREEN_ADJUST;
    s_last_adjust_ms = now_ms();
}

static void handle_event(const app_event_t *e)
{
    bool changed_setpoints = false, changed_mode = false, save = false;

    app_lock();
    switch (e->type) {
        case EVT_ENCODER_DELTA:
            apply_encoder(e->value);
            changed_setpoints = true; save = true;
            break;
        case EVT_BTN_MODE_SHORT:
            if (g_state.screen == UI_SCREEN_MENU) { g_state.screen = UI_SCREEN_HOME; }
            else { cycle_mode(); changed_mode = true; save = true; }
            break;
        case EVT_BTN_MENU_LONG:
            g_state.screen = (g_state.screen == UI_SCREEN_MENU) ? UI_SCREEN_HOME
                                                                : UI_SCREEN_MENU;
            break;
        case EVT_ENC_SW_SHORT:
            g_state.active_setpoint ^= 1;     /* toggle heat/cool target */
            g_state.screen = UI_SCREEN_ADJUST;
            s_last_adjust_ms = now_ms();
            break;
        case EVT_ENC_SW_LONG:
            g_state.screen = UI_SCREEN_HOME;
            break;
        case EVT_RESET_LONG:
            app_unlock();
            ESP_LOGW(TAG, "factory reset requested");
            app_matter_factory_reset();
            return;
        case EVT_MATTER_SET_MODE:
            g_state.cfg.mode = e->value; save = true; break;
        case EVT_MATTER_SET_HEAT:
            g_state.cfg.heat_set_c100 = clampi(e->value, HEAT_MIN, HEAT_MAX); save = true; break;
        case EVT_MATTER_SET_COOL:
            g_state.cfg.cool_set_c100 = clampi(e->value, COOL_MIN, COOL_MAX); save = true; break;
        case EVT_MATTER_SET_UNITS:
            g_state.cfg.fahrenheit = (e->value != 0); save = true; break;
        case EVT_MATTER_COMMISSIONED:
            g_state.commissioned = (e->value != 0);
            g_state.screen = UI_SCREEN_HOME;
            break;
    }
    app_config_t snapshot = g_state.cfg;
    app_unlock();

    /* Local changes -> notify Matter controllers. */
    if (changed_setpoints) app_matter_report_setpoints(snapshot.heat_set_c100, snapshot.cool_set_c100);
    if (changed_mode)      app_matter_report_mode(snapshot.mode);
    if (save)              app_nvs_save(&snapshot);
}

/* ---- UI task -------------------------------------------------------------- */

static void build_model(ui_model_t *m, char *code, int code_len)
{
    app_lock();
    m->temp_c100      = g_state.temp_c100;
    m->heat_set_c100  = g_state.cfg.heat_set_c100;
    m->cool_set_c100  = g_state.cfg.cool_set_c100;
    m->mode           = g_state.cfg.mode;
    m->fahrenheit     = g_state.cfg.fahrenheit;
    m->calling_heat   = g_state.calling_heat;
    m->calling_cool   = g_state.calling_cool;
    m->fan_on         = g_state.fan_on;
    m->fault          = g_state.fault;
    m->commissioned   = g_state.commissioned;
    m->thread_rssi    = g_state.thread_rssi;
    m->screen         = (ui_screen_t)g_state.screen;
    m->active_setpoint= g_state.active_setpoint;
    app_unlock();

    if (!m->commissioned) {
        app_matter_get_pairing_code(code, code_len);
        m->pairing_code = code;
    }
}

static void ui_task(void *arg)
{
    (void)arg;

    rotary_encoder_config_t ec = {
        .gpio_a = CONFIG_THERMO_PIN_ENC_A,
        .gpio_b = CONFIG_THERMO_PIN_ENC_B,
        .counts_per_detent = 4,
        .glitch_ns = 1000,
        .invert = false,
    };
    rotary_encoder_init(&ec);

    s_btn_q = xQueueCreate(8, sizeof(button_event_t));
    button_init(s_btn_q);
    button_cfg_t b_push  = { .gpio = CONFIG_THERMO_PIN_BTN_PUSH, .id = BTN_ID_PUSH,
                             .active_low = true, .long_press_ms = 3000 };
    button_cfg_t b_encsw = { .gpio = CONFIG_THERMO_PIN_ENC_SW, .id = BTN_ID_ENC_SW,
                             .active_low = true, .long_press_ms = 1000 };
    button_cfg_t b_reset = { .gpio = CONFIG_THERMO_PIN_RESET_BTN, .id = BTN_ID_RESET,
                             .active_low = true, .long_press_ms = 5000 };
    button_add(&b_push);
    button_add(&b_encsw);
    button_add(&b_reset);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);

    ui_oled_config_t oc = {
        .i2c_port = 0,
        .sda_gpio = CONFIG_THERMO_PIN_I2C_SDA,
        .scl_gpio = CONFIG_THERMO_PIN_I2C_SCL,
        .addr = CONFIG_THERMO_OLED_I2C_ADDR,
        .width = 128, .height = 64,
#if CONFIG_THERMO_OLED_SH1106
        .sh1106 = true,
#else
        .sh1106 = false,
#endif
        .i2c_hz = 400000,
    };
    ui_oled_init(&oc);
    app_lock();
    ui_oled_set_brightness((uint8_t)g_state.cfg.brightness);
    app_unlock();

    char code[24] = {0};
    int64_t last_render = 0;

    for (;;) {
        /* Encoder → event. */
        int d = rotary_encoder_get_delta();
        if (d != 0) app_post_event(EVT_ENCODER_DELTA, d);

        /* Drain pending app events (non-blocking). */
        app_event_t e;
        while (xQueueReceive(g_event_q, &e, 0) == pdTRUE) {
            handle_event(&e);
        }

        /* ADJUST auto-timeout back to HOME. */
        app_lock();
        if (g_state.screen == UI_SCREEN_ADJUST &&
            (now_ms() - s_last_adjust_ms) > CONFIG_THERMO_ADJUST_TIMEOUT_MS) {
            g_state.screen = UI_SCREEN_HOME;
        }
        app_unlock();

        /* Redraw at ~10 Hz. */
        if (now_ms() - last_render >= 100) {
            ui_model_t m = {0};
            build_model(&m, code, sizeof(code));
            ui_oled_render(&m);
            last_render = now_ms();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- entry ---------------------------------------------------------------- */

void app_control_start(void)
{
    xTaskCreate(sensor_task,  "sensor",  4096, NULL, 5, NULL);
    xTaskCreate(control_task, "control", 4096, NULL, 6, NULL);
    xTaskCreate(ui_task,      "ui",      5120, NULL, 4, NULL);
}

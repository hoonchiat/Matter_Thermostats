/*
 * ui_oled — SSD1306/SH1106 screen manager for the thermostat.
 *
 * Brings up the panel over I2C (via the esp_lcd_ssd1306 managed component) and
 * renders the screens described in docs/UI.md from a single view-model struct.
 *
 * Panel bring-up + the monochrome framebuffer + primitive drawing are provided.
 * Glyph/large-digit rendering is the documented integration point (bring your
 * own font table, or back this with LVGL) — see ui_oled_render().
 */
#ifndef UI_OLED_H
#define UI_OLED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_HOME = 0,
    UI_SCREEN_ADJUST,
    UI_SCREEN_MENU,
    UI_SCREEN_PAIRING,
    UI_SCREEN_FAULT,
} ui_screen_t;

/* The complete view-model the renderer needs; app fills this each redraw. */
typedef struct {
    ui_screen_t screen;
    int   temp_c100;        /* measured temperature, 0.01 C (ignored if fault)   */
    int   heat_set_c100;    /* heating setpoint, 0.01 C                           */
    int   cool_set_c100;    /* cooling setpoint, 0.01 C                           */
    int   mode;             /* thermo_mode_t                                     */
    bool  fahrenheit;       /* display units                                     */
    bool  calling_heat;
    bool  calling_cool;
    bool  fan_on;
    bool  fault;
    bool  commissioned;     /* Matter commissioned?                              */
    int   thread_rssi;      /* dBm, for the signal glyph (0 if unknown)          */
    int   active_setpoint;  /* 0 = heat, 1 = cool (which one ADJUST edits)        */
    const char *pairing_code;   /* manual pairing code string (PAIRING screen)   */
    const char *menu_title;     /* current menu item label (MENU screen)         */
    const char *menu_value;     /* current menu item value  (MENU screen)        */
} ui_model_t;

typedef struct {
    int  i2c_port;          /* 0 or 1 */
    int  sda_gpio;
    int  scl_gpio;
    int  addr;              /* 0x3C typical */
    int  width;             /* 128 */
    int  height;            /* 64 or 32 */
    bool sh1106;            /* true => SH1106, false => SSD1306 */
    int  i2c_hz;            /* e.g. 400000 */
} ui_oled_config_t;

/* Initialize the I2C bus + panel and clear the display. */
int  ui_oled_init(const ui_oled_config_t *cfg);

/* Render the given model to the framebuffer and flush it to the panel. */
void ui_oled_render(const ui_model_t *m);

/* Set panel contrast/brightness (0..255). */
void ui_oled_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* UI_OLED_H */

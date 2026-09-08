/*
 * ui_oled.c — panel bring-up + framebuffer + primitives + screen rendering.
 *
 * Panel init uses the ESP-IDF new I2C master driver and the esp_lcd_ssd1306
 * managed component. The framebuffer is a 1bpp, page-addressed buffer matching
 * SSD1306/SH1106 layout (each byte = 8 vertical pixels of one column/page).
 * Text is drawn with the built-in 5x7 font (font5x7.inc).
 *
 * The pure drawing/rendering path (framebuffer + font + screen composition)
 * compiles on a host PC when UI_OLED_HOST is defined, so the layout can be
 * previewed as ASCII without hardware (see test/host/preview_ui.c). Only the
 * I2C / esp_lcd panel bring-up and flush are compiled out in that mode.
 */
#include "ui_oled.h"

#include <string.h>
#include <stdio.h>

#ifdef UI_OLED_HOST
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
typedef void *i2c_master_bus_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#else
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#endif

#define TAG "ui_oled"
#define FB_MAX (128 * 64 / 8)

static struct {
    ui_oled_config_t cfg;
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    uint8_t fb[FB_MAX];
    uint32_t tick;          /* increments per render, drives the fan animation */
    bool ready;
} s;

/* ---- framebuffer primitives ---------------------------------------------- */

static void fb_clear(void) { memset(s.fb, 0, sizeof(s.fb)); }

static void fb_pixel(int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= s.cfg.width || y >= s.cfg.height) return;
    int page = y >> 3;
    uint8_t bit = 1u << (y & 7);
    uint8_t *p = &s.fb[page * s.cfg.width + x];
    if (on) *p |= bit; else *p &= ~bit;
}

static void fb_hline(int x0, int x1, int y, bool on)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; ++x) fb_pixel(x, y, on);
}

static void fb_rect(int x, int y, int w, int h, bool on)
{
    for (int i = 0; i < w; ++i) { fb_pixel(x + i, y, on); fb_pixel(x + i, y + h - 1, on); }
    for (int j = 0; j < h; ++j) { fb_pixel(x, y + j, on); fb_pixel(x + w - 1, y + j, on); }
}

static void fb_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            fb_pixel(x + i, y + j, on);
}

static void fb_flush(void)
{
    if (!s.ready) return;
#ifndef UI_OLED_HOST
    /* esp_lcd expects a bitmap covering the region; push the whole frame. */
    esp_lcd_panel_draw_bitmap(s.panel, 0, 0, s.cfg.width, s.cfg.height, s.fb);
#endif
}

/* ---- text (5x7 font) ----------------------------------------------------- */

#include "font5x7.inc"

/* Blit one glyph at (x,y) top-left, scaled, drawing pixels as `on`
 * (on=false clears — used to render inverse text over a highlight bar). */
static void draw_char_mode(int x, int y, char c, int scale, bool on)
{
    if (c >= 'a' && c <= 'z') c -= 32;           /* up-case */
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = ' ';
    const uint8_t *g = font5x7[(int)c - FONT5X7_FIRST];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1u << row)) {
                if (scale == 1) fb_pixel(x + col, y + row, on);
                else fb_fill_rect(x + col * scale, y + row * scale, scale, scale, on);
            }
        }
    }
}

/* Draw `str` at (x,y); returns the x just past the string. Advance = 6*scale. */
static int draw_text(int x, int y, const char *str, int scale)
{
    if (!str) return x;
    for (const char *c = str; *c; ++c) {
        draw_char_mode(x, y, *c, scale, true);
        x += 6 * scale;
    }
    return x;
}

/* Inverse text: clear glyph pixels (for drawing over a filled highlight bar). */
static void draw_text_inv(int x, int y, const char *str, int scale)
{
    if (!str) return;
    for (const char *c = str; *c; ++c) {
        draw_char_mode(x, y, *c, scale, false);
        x += 6 * scale;
    }
}

static int text_width(const char *str, int scale)
{
    return (int)strlen(str) * 6 * scale;
}

/* A small degree ring near (x,y) top-left (about 3x3). */
static void draw_degree(int x, int y)
{
    fb_pixel(x + 1, y, true);
    fb_pixel(x, y + 1, true);
    fb_pixel(x + 2, y + 1, true);
    fb_pixel(x + 1, y + 2, true);
}

/* ---- icons / decor ------------------------------------------------------- */

/* Rounded rectangle outline (corners clipped for a softer, modern look). */
static void draw_round_rect(int x, int y, int w, int h)
{
    fb_hline(x + 1, x + w - 2, y, true);
    fb_hline(x + 1, x + w - 2, y + h - 1, true);
    for (int j = 1; j < h - 1; ++j) { fb_pixel(x, y + j, true); fb_pixel(x + w - 1, y + j, true); }
}

/* Filled triangles used as heat (apex up) / cool (apex down) indicators. */
static void draw_tri_up(int cx, int y, int hgt)
{
    for (int r = 0; r < hgt; ++r) fb_hline(cx - r, cx + r, y + r, true);   /* narrow top */
}
static void draw_tri_down(int cx, int y, int hgt)
{
    for (int r = 0; r < hgt; ++r)
        fb_hline(cx - (hgt - 1 - r), cx + (hgt - 1 - r), y + r, true);     /* narrow bottom */
}

/* 8x8 icon blit (MSB = leftmost column). */
static void draw_icon8(int x, int y, const uint8_t ic[8])
{
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            if (ic[r] & (0x80 >> c)) fb_pixel(x + c, y + r, true);
}

/* Two fan frames; alternating them reads as a spinning blade. */
static const uint8_t ICON_FAN_X[8] = {
    0b10000001, 0b01000010, 0b00100100, 0b00011000,
    0b00011000, 0b00100100, 0b01000010, 0b10000001,
};
static const uint8_t ICON_FAN_PLUS[8] = {
    0b00011000, 0b00011000, 0b00011000, 0b11100111,
    0b11100111, 0b00011000, 0b00011000, 0b00011000,
};

/* Fan icon: animated when running, single static frame when idle. */
static void draw_fan(int x, int y, bool running)
{
    const uint8_t *f = (running && ((s.tick >> 2) & 1)) ? ICON_FAN_PLUS : ICON_FAN_X;
    draw_icon8(x, y, f);
}

/* Connectivity glyph: filled dot when commissioned, hollow ring otherwise. */
static void draw_conn(int x, int y, bool commissioned)
{
    /* 5x5 */
    fb_hline(x + 1, x + 3, y, true);
    fb_hline(x + 1, x + 3, y + 4, true);
    fb_pixel(x, y + 1, true); fb_pixel(x, y + 2, true); fb_pixel(x, y + 3, true);
    fb_pixel(x + 4, y + 1, true); fb_pixel(x + 4, y + 2, true); fb_pixel(x + 4, y + 3, true);
    if (commissioned) fb_fill_rect(x + 1, y + 1, 3, 3, true);
}

static const char *fan_abbrev(int fan_speed)
{
    switch (fan_speed) {
        case 1:  return "LO";
        case 2:  return "MD";
        case 3:  return "HI";
        default: return "AU";
    }
}

/* ---- unit / formatting helpers ------------------------------------------- */

static void fmt_temp(char *out, size_t n, int c100, bool fahrenheit)
{
    if (fahrenheit) {
        int f10 = (c100 * 9) / 500 + 320;   /* C*100 -> F*10 */
        snprintf(out, n, "%d.%d", f10 / 10, (f10 % 10 + 10) % 10);
    } else {
        snprintf(out, n, "%d.%d", c100 / 100, ((c100 % 100) / 10 + 10) % 10);
    }
}

static const char *mode_str(int mode)
{
    switch (mode) {
        case 0:  return "OFF";
        case 1:  return "HEAT";
        case 2:  return "COOL";
        case 3:  return "AUTO";
        case 4:  return "FAN";
        default: return "?";
    }
}

/* Draw a temperature value at (x,y) followed by a degree ring and C/F. */
static int draw_temp_unit(int x, int y, int c100, bool fahrenheit, int scale)
{
    char buf[12];
    fmt_temp(buf, sizeof(buf), c100, fahrenheit);
    x = draw_text(x, y, buf, scale);
    draw_degree(x + 1, y);
    x = draw_text(x + 5, y, fahrenheit ? "F" : "C", scale);
    return x;
}

/* ---- screen composition (geometry per docs/UI.md) ------------------------ */

/* Top status bar: mode (with heat/cool arrow), fan icon + speed, link dot. */
static void draw_status_bar(const ui_model_t *m)
{
    int x = 2;
    if (m->mode == 1) { draw_tri_up(x + 3, 2, 5); x += 9; }        /* HEAT */
    else if (m->mode == 2) { draw_tri_down(x + 3, 2, 5); x += 9; } /* COOL */
    draw_text(x, 2, mode_str(m->mode), 1);

    int rx = s.cfg.width;
    rx -= 5; draw_conn(rx, 2, m->commissioned);                    /* link dot */
    rx -= 2 + 8; draw_fan(rx, 1, m->fan_on);                       /* fan icon */
    rx -= 1 + text_width(fan_abbrev(m->fan_speed), 1);
    draw_text(rx, 2, fan_abbrev(m->fan_speed), 1);                 /* AU/LO/MD/HI */

    fb_hline(0, s.cfg.width - 1, 11, true);
}

/* Setpoint pill: single target for Heat/Cool/Off/Fan, both for Auto. */
static void draw_setpoint_pill(const ui_model_t *m)
{
    int y = 40, h = 14;
    draw_round_rect(4, y, s.cfg.width - 8, h);
    int ty = y + 4;
    if (m->mode == 3) {                                            /* AUTO: both */
        int x = 9;
        draw_tri_up(x + 2, ty + 1, 4); x += 8;
        x = draw_temp_unit(x, ty, m->heat_set_c100, m->fahrenheit, 1);
        x += 5;
        draw_tri_down(x + 2, ty + 1, 4); x += 8;
        draw_temp_unit(x, ty, m->cool_set_c100, m->fahrenheit, 1);
    } else {
        int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
        int x = draw_text(10, ty, "SET ", 1);
        draw_temp_unit(x, ty, set, m->fahrenheit, 1);
    }
    /* Calling indicator: a filled dot at the pill's right edge. */
    if (m->calling_heat || m->calling_cool)
        fb_fill_rect(s.cfg.width - 12, y + 5, 4, 4, true);
}

static void render_home(const ui_model_t *m)
{
    draw_status_bar(m);

    /* Big current temperature + unit, centered under the status bar. */
    char buf[12];
    fmt_temp(buf, sizeof(buf), m->temp_c100, m->fahrenheit);
    int w = text_width(buf, 3) + 6 /*deg*/ + 6 * 3 /*unit*/;
    draw_temp_unit((s.cfg.width - w) / 2, 15, m->temp_c100, m->fahrenheit, 3);

    draw_setpoint_pill(m);

    const char *rs = m->calling_heat ? "HEATING" :
                     (m->calling_cool ? "COOLING" : (m->fan_on ? "FAN ON" : "IDLE"));
    draw_text(2, 56, rs, 1);

    /* Bottom-right cluster: AWAY badge, then humidity (right-aligned). */
    int rx = s.cfg.width - 2;
    if (!m->occupied) {
        const char *aw = "AWAY";
        rx -= text_width(aw, 1);
        draw_text(rx, 56, aw, 1);
        rx -= 4;
    }
    if (m->humidity_valid) {
        char hb[16];
        int rh = (m->humidity_pct100 + 50) / 100;
        if (rh < 0)   rh = 0;
        if (rh > 100) rh = 100;
        snprintf(hb, sizeof(hb), "%d%%", rh);
        rx -= text_width(hb, 1);
        draw_text(rx, 56, hb, 1);
    }
}

static void render_adjust(const ui_model_t *m)
{
    draw_status_bar(m);
    const char *lbl = m->active_setpoint == 1
        ? (m->occupied ? "SET COOL" : "AWAY COOL")
        : (m->occupied ? "SET HEAT" : "AWAY HEAT");
    draw_text(2, 15, lbl, 1);

    int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
    char buf[12];
    fmt_temp(buf, sizeof(buf), set, m->fahrenheit);
    int w = text_width(buf, 3) + 6 + 6 * 3;
    draw_temp_unit((s.cfg.width - w) / 2, 26, set, m->fahrenheit, 3);

    /* Position bar within the allowed setpoint range. */
    int lo = (m->active_setpoint == 1) ? 1600 : 700;
    int hi = (m->active_setpoint == 1) ? 3200 : 3000;
    int frac = set <= lo ? 0 : set >= hi ? (s.cfg.width - 24) : (set - lo) * (s.cfg.width - 24) / (hi - lo);
    draw_round_rect(10, 54, s.cfg.width - 20, 9);
    fb_fill_rect(12, 56, frac, 5, true);
}

#define MENU_VISIBLE 5      /* rows that fit under the title (y 14..63, 10px each) */

static void render_menu(const ui_model_t *m)
{
    draw_text(2, 0, "SETTINGS", 1);
    fb_hline(0, s.cfg.width - 1, 10, true);

    int total = m->menu_count;
    bool bar = total > MENU_VISIBLE;             /* show a scrollbar? */
    int row_w = bar ? s.cfg.width - 4 : s.cfg.width;

    /* Scroll window: keep the selection centered where possible. */
    int first = m->menu_index - MENU_VISIBLE / 2;
    if (first > total - MENU_VISIBLE) first = total - MENU_VISIBLE;
    if (first < 0) first = 0;

    for (int r = 0; r < MENU_VISIBLE && (first + r) < total; ++r) {
        int i = first + r;
        int y = 14 + r * 10;
        const char *txt = m->menu_lines[i] ? m->menu_lines[i] : "";
        if (i == m->menu_index) {
            fb_fill_rect(0, y - 1, row_w, 10, true);   /* highlight bar */
            draw_text_inv(4, y, txt, 1);
        } else {
            draw_text(4, y, txt, 1);
        }
    }

    /* Scrollbar: track on the right + a proportional thumb. */
    if (bar) {
        int x = s.cfg.width - 2;
        int top = 13, h = s.cfg.height - top;      /* track height */
        for (int j = 0; j < h; ++j) fb_pixel(x, top + j, (j & 1) == 0);  /* dotted track */
        int thumb_h = h * MENU_VISIBLE / total; if (thumb_h < 6) thumb_h = 6;
        int thumb_y = top + (h - thumb_h) * first / (total - MENU_VISIBLE);
        fb_fill_rect(x - 1, thumb_y, 3, thumb_h, true);
    }
}

static void render_info(const ui_model_t *m)
{
    draw_text(2, 0, m->info_title ? m->info_title : "MATTER CODE", 1);
    fb_hline(0, s.cfg.width - 1, 10, true);
    if (m->info_line1) draw_text(2, 20, m->info_line1, 2);   /* big pairing code */
    if (m->info_line2) draw_text(2, 44, m->info_line2, 1);
}

static void render_pairing(const ui_model_t *m)
{
    draw_text(2, 0, "PAIR THERMOSTAT", 1);
    fb_rect(4, 14, 40, 40, true);                          /* QR placeholder box */
    draw_text(52, 20, "CODE:", 1);
    draw_text(52, 32, m->pairing_code ? m->pairing_code : "----", 1);
    draw_text(2, 56, "NEEDS THREAD BR", 1);
}

static void render_fault(const ui_model_t *m)
{
    (void)m;
    draw_text(2, 4, "! SENSOR FAULT", 1);
    draw_text(2, 24, "CHECK ROOM SENSOR", 1);
    draw_text(2, 40, "OUTPUTS DISABLED", 1);
}

/* ---- public API ---------------------------------------------------------- */

int ui_oled_init(const ui_oled_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s.cfg = *cfg;
    if (s.cfg.width == 0)  s.cfg.width = 128;
    if (s.cfg.height == 0) s.cfg.height = 64;
    if (s.cfg.i2c_hz == 0) s.cfg.i2c_hz = 400000;

#ifdef UI_OLED_HOST
    s.ready = true;
    fb_clear();
    return ESP_OK;
#else
    esp_err_t err;
    if (s.cfg.ext_bus) {
        /* Reuse a bus the app already created (shared with the SHT40). */
        s.bus = (i2c_master_bus_handle_t)s.cfg.ext_bus;
    } else {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = s.cfg.i2c_port,
            .sda_io_num = s.cfg.sda_gpio,
            .scl_io_num = s.cfg.scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&bus_cfg, &s.bus);
        if (err != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = s.cfg.addr,
        .scl_speed_hz = s.cfg.i2c_hz,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_i2c(s.bus, &io_cfg, &s.io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = (uint8_t)s.cfg.height };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    /* Note: for SH1106 use the SH1106 constructor from the same component. */
    err = esp_lcd_new_panel_ssd1306(s.io, &panel_cfg, &s.panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_reset(s.panel);
    esp_lcd_panel_init(s.panel);
    esp_lcd_panel_disp_on_off(s.panel, true);
    s.ready = true;

    fb_clear();
    fb_flush();
    ESP_LOGI(TAG, "OLED %dx%d @0x%02x ready", s.cfg.width, s.cfg.height, s.cfg.addr);
    return ESP_OK;
#endif /* UI_OLED_HOST */
}

void ui_oled_render(const ui_model_t *m)
{
    if (!m || !s.ready) return;
    s.tick++;
    fb_clear();
    /* Fault always wins (safety). Pairing replaces HOME while uncommissioned,
     * but the settings menu/info stay reachable so units/sensor type can be set
     * before pairing. */
    ui_screen_t scr;
    if (m->fault) scr = UI_SCREEN_FAULT;
    else if (!m->commissioned && m->screen == UI_SCREEN_HOME) scr = UI_SCREEN_PAIRING;
    else scr = (ui_screen_t)m->screen;
    switch (scr) {
        case UI_SCREEN_HOME:    render_home(m);    break;
        case UI_SCREEN_ADJUST:  render_adjust(m);  break;
        case UI_SCREEN_MENU:    render_menu(m);    break;
        case UI_SCREEN_INFO:    render_info(m);    break;
        case UI_SCREEN_PAIRING: render_pairing(m); break;
        case UI_SCREEN_FAULT:   render_fault(m);   break;
    }
    fb_flush();
}

void ui_oled_set_brightness(uint8_t level)
{
    if (!s.ready) return;
#ifndef UI_OLED_HOST
    /* SSD1306 contrast via the panel-io command path (0x81, level). */
    uint8_t cmd = 0x81;
    esp_lcd_panel_io_tx_param(s.io, cmd, &level, 1);
#else
    (void)level;
#endif
}

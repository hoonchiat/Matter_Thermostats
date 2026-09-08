/*
 * ui_oled.c — panel bring-up + framebuffer + primitives.
 *
 * Panel init uses the ESP-IDF new I2C master driver and the esp_lcd_ssd1306
 * managed component. The framebuffer is a 1bpp, page-addressed buffer matching
 * SSD1306/SH1106 layout (each byte = 8 vertical pixels of one column/page).
 *
 * TEXT RENDERING: intentionally a plug-in point. Wire a font table into
 * draw_text()/draw_big_number() (a compact 5x7 ASCII font, or LVGL). The layout
 * geometry for each screen is implemented per docs/UI.md; only glyph blitting is
 * stubbed, clearly marked with TODO(font).
 */
#include "ui_oled.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"

#define TAG "ui_oled"
#define FB_MAX (128 * 64 / 8)

static struct {
    ui_oled_config_t cfg;
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    uint8_t fb[FB_MAX];
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
    /* esp_lcd expects a bitmap covering the region; push the whole frame. */
    esp_lcd_panel_draw_bitmap(s.panel, 0, 0, s.cfg.width, s.cfg.height, s.fb);
}

/* ---- text (5x7 font) ----------------------------------------------------- */

#include "font5x7.inc"

/* Blit one glyph at (x,y) top-left, scaled. Lowercase is up-cased. */
static void draw_char(int x, int y, char c, int scale)
{
    if (c >= 'a' && c <= 'z') c -= 32;           /* up-case */
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = ' ';
    const uint8_t *g = font5x7[(int)c - FONT5X7_FIRST];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1u << row)) {
                if (scale == 1) fb_pixel(x + col, y + row, true);
                else fb_fill_rect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

/* Draw `str` at (x,y); returns the x just past the string. Advance = 6*scale. */
static int draw_text(int x, int y, const char *str, int scale)
{
    if (!str) return x;
    for (const char *c = str; *c; ++c) {
        draw_char(x, y, *c, scale);
        x += 6 * scale;
    }
    return x;
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

/* Render the large temperature centered horizontally at top y=cy. */
static void draw_big_number(int cx, int cy, const char *str)
{
    int scale = 3;
    int w = text_width(str, scale);
    draw_text(cx - w / 2, cy, str, scale);
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

static void render_home(const ui_model_t *m)
{
    draw_text(0, 0, mode_str(m->mode), 1);                 /* top-left: mode */
    draw_text(s.cfg.width - text_width(m->commissioned ? "NET" : "---", 1), 0,
              m->commissioned ? "NET" : "---", 1);

    /* Center the big temperature + unit together. */
    char buf[12];
    fmt_temp(buf, sizeof(buf), m->temp_c100, m->fahrenheit);
    int w = text_width(buf, 3) + 6 /*deg*/ + 6 * 3 /*unit*/;
    draw_temp_unit((s.cfg.width - w) / 2, 14, m->temp_c100, m->fahrenheit, 3);

    int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
    const char *lbl = (m->mode == 3) ? (m->active_setpoint == 1 ? "COOL " : "HEAT ") : "SET ";
    int x = draw_text(2, 44, lbl, 1);
    x = draw_temp_unit(x, 44, set, m->fahrenheit, 1);
    if (m->calling_heat)      draw_text(x + 4, 44, ">HEAT", 1);
    else if (m->calling_cool) draw_text(x + 4, 44, ">COOL", 1);

    fb_hline(0, s.cfg.width - 1, 54, true);
    const char *rs = m->calling_heat ? "HEATING" :
                     (m->calling_cool ? "COOLING" : (m->fan_on ? "FAN" : "IDLE"));
    draw_text(2, 56, rs, 1);
}

static void render_adjust(const ui_model_t *m)
{
    draw_text(2, 0, m->active_setpoint == 1 ? "SET COOLING" : "SET HEATING", 1);
    int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
    char buf[12];
    fmt_temp(buf, sizeof(buf), set, m->fahrenheit);
    int w = text_width(buf, 3) + 6 + 6 * 3;
    draw_temp_unit((s.cfg.width - w) / 2, 18, set, m->fahrenheit, 3);
    /* limit bar */
    fb_rect(10, 54, s.cfg.width - 20, 8, true);
    fb_fill_rect(12, 56, (s.cfg.width - 24) / 2, 4, true);
}

static void render_menu(const ui_model_t *m)
{
    draw_text(2, 0, "SETTINGS", 1);
    fb_hline(0, s.cfg.width - 1, 10, true);
    int y = 14;
    for (int i = 0; i < m->menu_count && i < UI_MENU_MAX; ++i) {
        bool sel = (i == m->menu_index);
        if (sel) {
            fb_fill_rect(0, y - 1, s.cfg.width, 10, true);  /* highlight bar */
        }
        /* Draw text; where the highlight is on, invert by clearing pixels. */
        int x0 = 4;
        const char *txt = m->menu_lines[i] ? m->menu_lines[i] : "";
        if (!sel) {
            draw_text(x0, y, txt, 1);
        } else {
            /* Simple inverse: draw text then knock it back out of the bar. */
            for (const char *c = txt; *c; ++c) {
                char cc = *c;
                if (cc >= 'a' && cc <= 'z') cc -= 32;
                if (cc < FONT5X7_FIRST || cc > FONT5X7_LAST) cc = ' ';
                const uint8_t *g = font5x7[(int)cc - FONT5X7_FIRST];
                for (int col = 0; col < 5; ++col)
                    for (int row = 0; row < 7; ++row)
                        if (g[col] & (1u << row)) fb_pixel(x0 + col, y + row, false);
                x0 += 6;
            }
        }
        y += 10;
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

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = s.cfg.i2c_port,
        .sda_io_num = s.cfg.sda_gpio,
        .scl_io_num = s.cfg.scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s.bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }

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
}

void ui_oled_render(const ui_model_t *m)
{
    if (!m || !s.ready) return;
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
    /* SSD1306 contrast via the panel-io command path (0x81, level). */
    uint8_t cmd = 0x81;
    esp_lcd_panel_io_tx_param(s.io, cmd, &level, 1);
}

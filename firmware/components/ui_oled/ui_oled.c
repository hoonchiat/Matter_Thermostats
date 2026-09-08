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

/* ---- text (PLUG-IN POINT) ------------------------------------------------ */

/* TODO(font): blit `str` at (x,y) using a 5x7 font (scale x1). Return end x.
 * For now this reserves the layout box so callers compose correct geometry. */
static int draw_text(int x, int y, const char *str, int scale)
{
    if (!str) return x;
    int adv = (5 * scale + 1);
    /* Placeholder: underline where text will render so screens are inspectable. */
    for (const char *c = str; *c; ++c) {
        fb_hline(x, x + 5 * scale - 1, y + 7 * scale, true);
        x += adv;
    }
    return x;
}

/* TODO(font): render the large temperature (e.g. "21.4") centered. */
static void draw_big_number(int cx, int cy, const char *str)
{
    int scale = 3;
    int w = (int)strlen(str) * (5 * scale + 1);
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

/* ---- screen composition (geometry per docs/UI.md) ------------------------ */

static void render_home(const ui_model_t *m)
{
    char buf[16];
    draw_text(0, 0, mode_str(m->mode), 1);                 /* top-left: mode */
    draw_text(s.cfg.width - 18, 0, m->commissioned ? "net" : "---", 1);

    fmt_temp(buf, sizeof(buf), m->temp_c100, m->fahrenheit);
    draw_big_number(s.cfg.width / 2, 16, buf);             /* center: big temp */

    int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
    fmt_temp(buf, sizeof(buf), set, m->fahrenheit);
    char line[24];
    snprintf(line, sizeof(line), "Set %s%s", buf,
             m->calling_heat ? " ^" : (m->calling_cool ? " v" : ""));
    draw_text(2, 44, line, 1);

    fb_hline(0, s.cfg.width - 1, 54, true);
    const char *rs = m->calling_heat ? "heating" :
                     (m->calling_cool ? "cooling" : (m->fan_on ? "fan" : "idle"));
    draw_text(2, 56, rs, 1);
}

static void render_adjust(const ui_model_t *m)
{
    char buf[16];
    draw_text(2, 0, m->active_setpoint == 1 ? "Set cooling" : "Set heating", 1);
    int set = (m->active_setpoint == 1) ? m->cool_set_c100 : m->heat_set_c100;
    fmt_temp(buf, sizeof(buf), set, m->fahrenheit);
    draw_big_number(s.cfg.width / 2, 20, buf);
    /* limit bar */
    fb_rect(10, 54, s.cfg.width - 20, 8, true);
    fb_fill_rect(12, 56, (s.cfg.width - 24) / 2, 4, true);
}

static void render_menu(const ui_model_t *m)
{
    draw_text(2, 0, "Settings", 1);
    fb_hline(0, s.cfg.width - 1, 10, true);
    draw_text(4, 20, m->menu_title ? m->menu_title : "", 1);
    draw_text(4, 36, m->menu_value ? m->menu_value : "", 1);
}

static void render_pairing(const ui_model_t *m)
{
    draw_text(2, 0, "Pair thermostat", 1);
    fb_rect(4, 14, 40, 40, true);                          /* QR placeholder box */
    draw_text(52, 20, "Code:", 1);
    draw_text(52, 32, m->pairing_code ? m->pairing_code : "----", 1);
    draw_text(2, 56, "Needs Thread BR", 1);
}

static void render_fault(const ui_model_t *m)
{
    (void)m;
    draw_text(2, 4, "! SENSOR FAULT", 1);
    draw_text(2, 24, "Check room sensor", 1);
    draw_text(2, 40, "Outputs disabled", 1);
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
    ui_screen_t scr = m->fault ? UI_SCREEN_FAULT :
                      (!m->commissioned ? UI_SCREEN_PAIRING : m->screen);
    switch (scr) {
        case UI_SCREEN_HOME:    render_home(m);    break;
        case UI_SCREEN_ADJUST:  render_adjust(m);  break;
        case UI_SCREEN_MENU:    render_menu(m);    break;
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

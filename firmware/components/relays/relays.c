#include "relays.h"

#include "esp_log.h"
#include "driver/gpio.h"

#define TAG "relays"

static relays_config_t s;

static void drive(int gpio, bool on)
{
    if (gpio < 0) return;
    int level = s.active_high ? (on ? 1 : 0) : (on ? 0 : 1);
    gpio_set_level(gpio, level);
}

static void configure(int gpio)
{
    if (gpio < 0) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static bool has_speed_taps(void)
{
    return s.gpio_g_low >= 0 || s.gpio_g_med >= 0 || s.gpio_g_high >= 0;
}

int relays_init(const relays_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s = *cfg;
    configure(s.gpio_w);
    configure(s.gpio_y);
    configure(s.gpio_g);
    configure(s.gpio_ob);
    configure(s.gpio_g_low);
    configure(s.gpio_g_med);
    configure(s.gpio_g_high);
    relays_all_off();
    ESP_LOGI(TAG, "init W=%d Y=%d G=%d OB=%d taps(L/M/H)=%d/%d/%d active_high=%d",
             s.gpio_w, s.gpio_y, s.gpio_g, s.gpio_ob,
             s.gpio_g_low, s.gpio_g_med, s.gpio_g_high, s.active_high);
    return ESP_OK;
}

void relays_apply(const relays_state_t *st)
{
    if (!st) return;
    drive(s.gpio_w, st->w);
    drive(s.gpio_y, st->y);
    drive(s.gpio_g, st->g);
    drive(s.gpio_ob, st->ob);

    /* Multi-speed blower taps: energize exactly the one matching the level. */
    if (has_speed_taps()) {
        drive(s.gpio_g_low,  st->fan_level == 1);
        drive(s.gpio_g_med,  st->fan_level == 2);
        drive(s.gpio_g_high, st->fan_level == 3);
    }
}

void relays_all_off(void)
{
    drive(s.gpio_w, false);
    drive(s.gpio_y, false);
    drive(s.gpio_g, false);
    drive(s.gpio_ob, false);
    drive(s.gpio_g_low, false);
    drive(s.gpio_g_med, false);
    drive(s.gpio_g_high, false);
}

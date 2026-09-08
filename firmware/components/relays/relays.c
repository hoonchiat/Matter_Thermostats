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

int relays_init(const relays_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s = *cfg;
    configure(s.gpio_w);
    configure(s.gpio_y);
    configure(s.gpio_g);
    configure(s.gpio_ob);
    relays_all_off();
    ESP_LOGI(TAG, "init W=%d Y=%d G=%d OB=%d active_high=%d",
             s.gpio_w, s.gpio_y, s.gpio_g, s.gpio_ob, s.active_high);
    return ESP_OK;
}

void relays_apply(const relays_state_t *st)
{
    if (!st) return;
    drive(s.gpio_w, st->w);
    drive(s.gpio_y, st->y);
    drive(s.gpio_g, st->g);
    drive(s.gpio_ob, st->ob);
}

void relays_all_off(void)
{
    drive(s.gpio_w, false);
    drive(s.gpio_y, false);
    drive(s.gpio_g, false);
    drive(s.gpio_ob, false);
}

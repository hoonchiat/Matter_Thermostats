/* occupancy.c — ESP-IDF GPIO occupancy/PIR sensor with vacancy timeout. */
#include "occupancy.h"

#include "esp_log.h"
#include "driver/gpio.h"

#define TAG "occupancy"

static struct {
    occupancy_config_t cfg;
    bool     present;
    uint64_t last_motion_ms;
} s;

int occupancy_init(const occupancy_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s.cfg = *cfg;
    s.last_motion_ms = 0;

    if (cfg->gpio < 0) {
        s.present = false;
        ESP_LOGI(TAG, "no occupancy sensor (manual Home/Away)");
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->gpio,
        .mode = GPIO_MODE_INPUT,
        /* Idle level opposite the active level, so an unconnected pin reads "no motion". */
        .pull_up_en   = cfg->active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = cfg->active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s.present = true;
    ESP_LOGI(TAG, "occupancy sensor gpio=%d timeout=%ds active_%s",
             cfg->gpio, cfg->vacancy_timeout_s, cfg->active_high ? "high" : "low");
    return ESP_OK;
}

bool occupancy_sensor_present(void) { return s.present; }

bool occupancy_poll(uint64_t now_ms)
{
    if (!s.present) return true;                       /* no sensor -> occupied */

    int lvl = gpio_get_level(s.cfg.gpio);
    bool motion = s.cfg.active_high ? (lvl != 0) : (lvl == 0);
    if (motion || s.last_motion_ms == 0) {
        s.last_motion_ms = now_ms;                     /* seed on first poll too */
    }
    return occupancy_from_motion(now_ms, s.last_motion_ms, s.cfg.vacancy_timeout_s);
}

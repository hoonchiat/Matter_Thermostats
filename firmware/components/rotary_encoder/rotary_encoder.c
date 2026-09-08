#include "rotary_encoder.h"

#include "esp_log.h"
#include "driver/pulse_cnt.h"

#define TAG "rotary_encoder"
#define PCNT_HIGH_LIMIT  1000
#define PCNT_LOW_LIMIT  -1000

static struct {
    pcnt_unit_handle_t unit;
    int counts_per_detent;
    bool invert;
    int last_raw;           /* accumulated count at last read (with wrap tracked) */
    int remainder;          /* sub-detent leftover pulses */
} s;

int rotary_encoder_init(const rotary_encoder_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s.counts_per_detent = cfg->counts_per_detent > 0 ? cfg->counts_per_detent : 4;
    s.invert = cfg->invert;
    s.last_raw = 0;
    s.remainder = 0;

    pcnt_unit_config_t unit_cfg = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit  = PCNT_LOW_LIMIT,
        .flags.accum_count = true,   /* accumulate on watch-point wrap */
    };
    esp_err_t err = pcnt_new_unit(&unit_cfg, &s.unit);
    if (err != ESP_OK) return err;

    pcnt_glitch_filter_config_t filter = {
        .max_glitch_ns = cfg->glitch_ns > 0 ? cfg->glitch_ns : 1000,
    };
    pcnt_unit_set_glitch_filter(s.unit, &filter);

    /* Watch points at the limits so accum_count keeps a continuous total. */
    pcnt_unit_add_watch_point(s.unit, PCNT_HIGH_LIMIT);
    pcnt_unit_add_watch_point(s.unit, PCNT_LOW_LIMIT);

    /* Channel A: edge on A, level from B. */
    pcnt_chan_config_t ca = { .edge_gpio_num = cfg->gpio_a, .level_gpio_num = cfg->gpio_b };
    pcnt_channel_handle_t chan_a = NULL;
    pcnt_new_channel(s.unit, &ca, &chan_a);
    pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    /* Channel B: edge on B, level from A (full x4 quadrature decoding). */
    pcnt_chan_config_t cb = { .edge_gpio_num = cfg->gpio_b, .level_gpio_num = cfg->gpio_a };
    pcnt_channel_handle_t chan_b = NULL;
    pcnt_new_channel(s.unit, &cb, &chan_b);
    pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(s.unit);
    pcnt_unit_clear_count(s.unit);
    pcnt_unit_start(s.unit);

    ESP_LOGI(TAG, "init: A=%d B=%d cpd=%d", cfg->gpio_a, cfg->gpio_b, s.counts_per_detent);
    return ESP_OK;
}

int rotary_encoder_get_delta(void)
{
    int raw = 0;
    if (pcnt_unit_get_count(s.unit, &raw) != ESP_OK) return 0;

    int diff = raw - s.last_raw;
    s.last_raw = raw;
    if (s.invert) diff = -diff;

    diff += s.remainder;
    int detents = diff / s.counts_per_detent;
    s.remainder = diff - detents * s.counts_per_detent;
    return detents;
}

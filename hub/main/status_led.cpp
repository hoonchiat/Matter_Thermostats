/* status_led.cpp - see status_led.h. WS2812 RGB LED driven over RMT. */
#include "status_led.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LED_GPIO    8       /* ESP32-C6-DevKitC-1 onboard addressable RGB LED */
#define LED_BRIGHT  0x30    /* 0..255 per channel (kept low to avoid glare)   */

static const char *TAG = "led";
static rmt_channel_handle_t s_chan  = nullptr;
static rmt_encoder_handle_t s_enc   = nullptr;
static esp_timer_handle_t   s_timer = nullptr;
static volatile led_state_t s_state = LED_BOOTING;

/* Push one WS2812 pixel (order on the wire is G,R,B). Non-blocking. */
static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_chan || !s_enc) return;
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = {};
    tx.loop_count = 0;
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
}

/* 250 ms tick: blink for PAIRING, refresh the solid colour otherwise. */
static void led_tick(void *)
{
    static bool on = false; on = !on;
    switch (s_state) {
    case LED_OPERATING: led_write(0, LED_BRIGHT, 0); break;                 /* green  */
    case LED_PAIRING:   led_write(on ? LED_BRIGHT : 0, on ? LED_BRIGHT : 0, 0); break; /* amber blink */
    case LED_BOOTING:
    default:            led_write(0, 0, LED_BRIGHT); break;                 /* blue   */
    }
}

void status_led_init(void)
{
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num        = (gpio_num_t)LED_GPIO;
    cfg.clk_src         = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz   = 10 * 1000 * 1000;   /* 10 MHz -> 0.1 us per tick */
    cfg.mem_block_symbols = 64;
    cfg.trans_queue_depth = 4;
    if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "RMT TX channel init failed - status LED disabled");
        return;
    }

    /* WS2812 bit timing at 0.1 us ticks: 0 -> 0.3us hi/0.9us lo, 1 -> 0.9/0.3. */
    rmt_bytes_encoder_config_t enc = {};
    enc.bit0.level0 = 1; enc.bit0.duration0 = 3; enc.bit0.level1 = 0; enc.bit0.duration1 = 9;
    enc.bit1.level0 = 1; enc.bit1.duration0 = 9; enc.bit1.level1 = 0; enc.bit1.duration1 = 3;
    enc.flags.msb_first = 1;
    if (rmt_new_bytes_encoder(&enc, &s_enc) != ESP_OK) {
        ESP_LOGW(TAG, "RMT encoder init failed - status LED disabled");
        return;
    }
    rmt_enable(s_chan);

    esp_timer_create_args_t ta = {};
    ta.callback = led_tick;
    ta.name     = "status_led";
    esp_timer_create(&ta, &s_timer);
    esp_timer_start_periodic(s_timer, 250 * 1000);   /* 250 ms */
    led_tick(nullptr);                               /* show initial state now */
    ESP_LOGI(TAG, "Status LED on GPIO%d (blue=boot, green=ready, amber=pairing)", LED_GPIO);
}

void status_led_set_state(led_state_t st)
{
    s_state = st;
    if (s_timer) led_tick(nullptr);   /* reflect the change immediately */
}

#include "button.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define TAG "button"
#define MAX_BUTTONS 4
#define POLL_PERIOD_MS 5
#define DEBOUNCE_MS 30

typedef struct {
    button_cfg_t cfg;
    int      last_level;
    int      stable_level;      /* debounced logical level (0/1 raw) */
    int64_t  changed_ms;        /* when raw level last changed */
    int64_t  press_start_ms;
    bool     pressed;
    bool     long_fired;
    bool     in_use;
} btn_t;

static btn_t s_btns[MAX_BUTTONS];
static QueueHandle_t s_queue;
static esp_timer_handle_t s_timer;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void post(int id, button_event_type_t type)
{
    button_event_t ev = { .id = id, .type = type };
    if (s_queue) xQueueSend(s_queue, &ev, 0);
}

static void poll_cb(void *arg)
{
    (void)arg;
    int64_t t = now_ms();
    for (int i = 0; i < MAX_BUTTONS; ++i) {
        btn_t *b = &s_btns[i];
        if (!b->in_use) continue;

        int raw = gpio_get_level(b->cfg.gpio);
        if (raw != b->last_level) {
            b->last_level = raw;
            b->changed_ms = t;                 /* start settling window */
        }
        if ((t - b->changed_ms) >= DEBOUNCE_MS && raw != b->stable_level) {
            b->stable_level = raw;
            bool now_pressed = b->cfg.active_low ? (raw == 0) : (raw == 1);
            if (now_pressed && !b->pressed) {
                b->pressed = true;
                b->long_fired = false;
                b->press_start_ms = t;
            } else if (!now_pressed && b->pressed) {
                b->pressed = false;
                if (!b->long_fired) post(b->cfg.id, BUTTON_EVENT_SHORT);
            }
        }
        if (b->pressed && !b->long_fired &&
            (t - b->press_start_ms) >= (int64_t)b->cfg.long_press_ms) {
            b->long_fired = true;
            post(b->cfg.id, BUTTON_EVENT_LONG);
        }
    }
}

int button_init(QueueHandle_t event_queue)
{
    memset(s_btns, 0, sizeof(s_btns));
    s_queue = event_queue;

    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .name = "button_poll",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(s_timer, POLL_PERIOD_MS * 1000);
}

int button_add(const button_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < MAX_BUTTONS; ++i) {
        if (s_btns[i].in_use) continue;

        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en   = cfg->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = cfg->active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);

        btn_t *b = &s_btns[i];
        b->cfg = *cfg;
        b->last_level = gpio_get_level(cfg->gpio);
        b->stable_level = b->last_level;
        b->changed_ms = now_ms();
        b->pressed = false;
        b->long_fired = false;
        b->in_use = true;
        ESP_LOGI(TAG, "add button id=%d gpio=%d", cfg->id, cfg->gpio);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "no free button slot");
    return ESP_ERR_NO_MEM;
}

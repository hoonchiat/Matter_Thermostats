/*
 * occupancy.cpp - local occupancy auto-off for the RGB light (see occupancy.h).
 *
 * PIR/occupancy sensor on OCC_PIN (active-HIGH presence, internal pull-up). A 1 s
 * task samples presence; a rising-edge ISR also stamps presence to catch brief
 * pulses. When the light is ON and no presence has been seen for `timeout_min`
 * minutes, it sets OnOff=false (turns the LED off AND reports to the hub, exactly
 * like the toggle button). No sensor wired -> pin pulled HIGH -> always "present"
 * -> never auto-offs (normal light).
 */
#include "occupancy.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <nvs.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "occupancy";

/* PIR/occupancy input. Free on the C6 light (LED=8, BOOT=9). Wire the sensor's
 * digital OUT here, plus its VCC/GND. Presence = HIGH. */
#define OCC_PIN            GPIO_NUM_10
#define POLL_MS            1000
#define DEFAULT_TIMEOUT_MIN 10
#define NVS_NS             "occ"
#define NVS_KEY            "tmin"

static uint16_t          s_timeout_min = DEFAULT_TIMEOUT_MIN;
static uint16_t          s_light_ep    = 0;
static volatile int64_t  s_last_present_us = 0;

static void IRAM_ATTR occ_isr(void *arg)
{
    s_last_present_us = esp_timer_get_time();   /* rising edge = presence */
}

static bool light_is_on(void)
{
    attribute_t *a = attribute::get(s_light_ep, OnOff::Id, OnOff::Attributes::OnOff::Id);
    if (!a) return false;
    esp_matter_attr_val_t v = esp_matter_invalid(NULL);
    attribute::get_val(a, &v);
    return v.val.b;
}

static void light_turn_off(void)
{
    esp_matter_attr_val_t v = esp_matter_bool(false);
    /* same path the toggle button uses: updates the attribute, drives the LED,
     * and reports OFF to the hub's subscription. */
    attribute::update(s_light_ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &v);
    ESP_LOGI(TAG, "no presence for %u min -> light OFF", s_timeout_min);
}

static void occ_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(OCC_PIN)) s_last_present_us = esp_timer_get_time();  /* present now */

        int64_t idle_us = esp_timer_get_time() - s_last_present_us;
        if (idle_us >= (int64_t)s_timeout_min * 60 * 1000000LL && light_is_on())
            light_turn_off();
    }
}

/* ---- NVS-backed period -------------------------------------------------- */
static void load_timeout(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t v = 0;
        if (nvs_get_u16(h, NVS_KEY, &v) == ESP_OK && v >= 1 && v <= 1440) s_timeout_min = v;
        nvs_close(h);
    }
}

void occupancy_set_timeout_min(uint16_t minutes)
{
    if (minutes < 1) minutes = 1;
    if (minutes > 1440) minutes = 1440;
    s_timeout_min = minutes;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY, minutes); nvs_commit(h); nvs_close(h);
    }
    /* count from now, so a fresh period starts at the moment it is (re)configured */
    s_last_present_us = esp_timer_get_time();
    ESP_LOGI(TAG, "auto-off period set to %u min", minutes);
}

uint16_t occupancy_get_timeout_min(void) { return s_timeout_min; }

void occupancy_start(uint16_t light_endpoint_id)
{
    s_light_ep = light_endpoint_id;
    load_timeout();
    s_last_present_us = esp_timer_get_time();

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << OCC_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;    /* no sensor -> reads HIGH -> "present" -> feature off */
    io.intr_type    = GPIO_INTR_POSEDGE;
    gpio_config(&io);
    gpio_install_isr_service(0);              /* harmless if iot_button already installed it */
    gpio_isr_handler_add(OCC_PIN, occ_isr, nullptr);

    xTaskCreate(occ_task, "occupancy", 3072, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "occupancy auto-off started (GPIO%d, active-high+pullup, %u min)", OCC_PIN, s_timeout_min);
}

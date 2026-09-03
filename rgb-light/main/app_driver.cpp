/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <esp_timer.h>

#include <device.h>
#include <led_driver.h>
#include <button_gpio.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t light_endpoint_id;

// Global variables to store current XY color coordinates
static uint16_t current_x = 0;
static uint16_t current_y = 0;

// The onboard RGB LED handle, kept so the pairing indicator can drive it directly.
static led_driver_handle_t s_led = NULL;

/* ---- pairing-mode slow flash ------------------------------------------- *
 * While a commissioning window is open the bulb blinks blue ~0.7 s on / 0.7 s
 * off so it's obvious it's waiting to pair; on close we restore the real state. */
static esp_timer_handle_t s_pair_timer = NULL;
static bool s_pair_phase = false;
static bool s_pairing    = false;

static void pair_blink_cb(void *)
{
    if (!s_led) return;
    s_pair_phase = !s_pair_phase;
    if (s_pair_phase) {
        led_driver_set_hue(s_led, 210);          /* blue  (0..360) */
        led_driver_set_saturation(s_led, 100);   /* full  (0..100) */
        led_driver_set_brightness(s_led, 25);    /* dim   (0..100) */
        led_driver_set_power(s_led, true);
    } else {
        led_driver_set_power(s_led, false);
    }
}

void app_driver_pairing_indicate(bool pairing)
{
    if (pairing == s_pairing) return;
    s_pairing = pairing;
    if (pairing) {
        if (!s_pair_timer) {
            const esp_timer_create_args_t args = { .callback = pair_blink_cb, .name = "pairblink" };
            esp_timer_create(&args, &s_pair_timer);
        }
        s_pair_phase = false;
        esp_timer_start_periodic(s_pair_timer, 700 * 1000);   /* 700 ms -> slow flash */
        ESP_LOGI(TAG, "Pairing mode: LED slow-flashing (blue)");
    } else {
        if (s_pair_timer) esp_timer_stop(s_pair_timer);
        ESP_LOGI(TAG, "Pairing ended: restoring the light");
        app_driver_light_set_defaults(light_endpoint_id);
    }
}

/* ---- factory-reset-to-pair gesture: hold BOOT for 15 s ------------------ *
 * Nothing happens for the first 10 s; then the LED flashes YELLOW at a ~1 s
 * interval for 5 s (the confirm window) while the button is still held.
 * Releasing any time before 15 s cancels and restores the bulb. At 15 s the
 * device factory-resets (erases the Matter fabric) and reboots into pairing. */
#define RESET_CONFIRM_MS 10000   /* yellow confirm flash starts here          */
#define RESET_FIRE_MS    15000   /* factory reset fires here (10 s + 5 s)     */
#define RESET_BLINK_MS     500   /* yellow half-period -> ~1 s flash interval */

static esp_timer_handle_t s_reset_timer = NULL;
static int64_t s_reset_start_us   = 0;
static bool    s_reset_confirming = false;

static void reset_restore_led(void)
{
    /* Bring the bulb back to its real on/off/colour/brightness. */
    app_driver_light_set_defaults(light_endpoint_id);
}

static void reset_fire_work(intptr_t) { esp_matter::factory_reset(); }

static void reset_tick_cb(void *)
{
    uint32_t held = (uint32_t)((esp_timer_get_time() - s_reset_start_us) / 1000);
    if (held >= RESET_FIRE_MS) {
        esp_timer_stop(s_reset_timer);
        s_reset_confirming = false;
        ESP_LOGW(TAG, "Reset gesture complete (15 s) -> factory reset + re-pair");
        chip::DeviceLayer::PlatformMgr().ScheduleWork(reset_fire_work, 0);
        return;
    }
    if (held >= RESET_CONFIRM_MS) {
        if (!s_reset_confirming) {
            s_reset_confirming = true;
            ESP_LOGW(TAG, "Reset gesture: keep holding to factory-reset (release to cancel)");
        }
        bool on = (((held - RESET_CONFIRM_MS) / RESET_BLINK_MS) & 1U) == 0U;
        if (s_led) {
            if (on) {
                led_driver_set_hue(s_led, 60);          /* yellow (0..360) */
                led_driver_set_saturation(s_led, 100);
                led_driver_set_brightness(s_led, 40);
                led_driver_set_power(s_led, true);
            } else {
                led_driver_set_power(s_led, false);
            }
        }
    }
}

static void reset_press_down_cb(void *, void *)
{
    s_reset_start_us   = esp_timer_get_time();
    s_reset_confirming = false;
    if (!s_reset_timer) {
        const esp_timer_create_args_t args = { .callback = reset_tick_cb, .name = "resethold" };
        esp_timer_create(&args, &s_reset_timer);
    }
    esp_timer_stop(s_reset_timer);
    esp_timer_start_periodic(s_reset_timer, 250 * 1000);   /* 250 ms tick */
}

static void reset_press_up_cb(void *, void *)
{
    if (s_reset_timer) esp_timer_stop(s_reset_timer);
    if (s_reset_confirming) {
        s_reset_confirming = false;
        ESP_LOGI(TAG, "Reset gesture cancelled (released before 15 s)");
        reset_restore_led();
    }
}

/* Do any conversions/remapping for the actual value here */
static esp_err_t app_driver_light_set_power(led_driver_handle_t handle, esp_matter_attr_val_t *val)
{
    return led_driver_set_power(handle, val->val.b);
}

static esp_err_t app_driver_light_set_brightness(led_driver_handle_t handle, esp_matter_attr_val_t *val)
{
    int value = REMAP_TO_RANGE(val->val.u8, MATTER_BRIGHTNESS, STANDARD_BRIGHTNESS);
    return led_driver_set_brightness(handle, value);
}

static esp_err_t app_driver_light_set_hue(led_driver_handle_t handle, esp_matter_attr_val_t *val)
{
    int value = REMAP_TO_RANGE(val->val.u8, MATTER_HUE, STANDARD_HUE);
    return led_driver_set_hue(handle, value);
}

static esp_err_t app_driver_light_set_saturation(led_driver_handle_t handle, esp_matter_attr_val_t *val)
{
    int value = REMAP_TO_RANGE(val->val.u8, MATTER_SATURATION, STANDARD_SATURATION);
    return led_driver_set_saturation(handle, value);
}

static esp_err_t app_driver_light_set_temperature(led_driver_handle_t handle, esp_matter_attr_val_t *val)
{
    uint32_t value = REMAP_TO_RANGE_INVERSE(val->val.u16, STANDARD_TEMPERATURE_FACTOR);
    return led_driver_set_temperature(handle, value);
}

static esp_err_t app_driver_light_set_xy(led_driver_handle_t handle, uint16_t x, uint16_t y)
{
    return led_driver_set_xy(handle, x, y);
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = light_endpoint_id;
    uint32_t cluster_id = OnOff::Id;
    uint32_t attribute_id = OnOff::Attributes::OnOff::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(attribute, &val);
    val.val.b = !val.val.b;
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == light_endpoint_id) {
        led_driver_handle_t handle = (led_driver_handle_t)driver_handle;
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                err = app_driver_light_set_power(handle, val);
            }
        } else if (cluster_id == LevelControl::Id) {
            if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
                err = app_driver_light_set_brightness(handle, val);
            }
        } else if (cluster_id == ColorControl::Id) {
            if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
                err = app_driver_light_set_hue(handle, val);
            } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
                err = app_driver_light_set_saturation(handle, val);
            } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
                err = app_driver_light_set_temperature(handle, val);
            } else if (attribute_id == ColorControl::Attributes::CurrentX::Id) {
                current_x = val->val.u16;
                err = app_driver_light_set_xy(handle, current_x, current_y);
            } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
                current_y = val->val.u16;
                err = app_driver_light_set_xy(handle, current_x, current_y);
            }
        }
    }
    return err;
}

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    void *priv_data = endpoint::get_priv_data(endpoint_id);
    led_driver_handle_t handle = (led_driver_handle_t)priv_data;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);

    /* Setting brightness */
    attribute_t *attribute = attribute::get(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    attribute::get_val(attribute, &val);
    err |= app_driver_light_set_brightness(handle, &val);

    /* Setting color */
    attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    attribute::get_val(attribute, &val);
    if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
        /* Setting hue */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
        attribute::get_val(attribute, &val);
        err |= app_driver_light_set_hue(handle, &val);
        /* Setting saturation */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
        attribute::get_val(attribute, &val);
        err |= app_driver_light_set_saturation(handle, &val);
    } else if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kColorTemperature) {
        /* Setting temperature */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
        attribute::get_val(attribute, &val);
        err |= app_driver_light_set_temperature(handle, &val);
    } else if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
        /* Setting XY coordinates */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
        attribute::get_val(attribute, &val);
        current_x = val.val.u16;
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
        attribute::get_val(attribute, &val);
        current_y = val.val.u16;
        err |= app_driver_light_set_xy(handle, current_x, current_y);
    } else {
        ESP_LOGE(TAG, "Color mode not supported");
    }

    /* Setting power */
    attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::get_val(attribute, &val);
    err |= app_driver_light_set_power(handle, &val);

    return err;
}

app_driver_handle_t app_driver_light_init()
{
    /* Initialize led */
    led_driver_config_t config = led_driver_get_config();
    led_driver_handle_t handle = led_driver_init(&config);
    s_led = handle;
    return (app_driver_handle_t)handle;
}

app_driver_handle_t app_driver_button_init()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    /* Short click toggles on/off. A 5 s hold is handled separately by
     * app_reset_button_register() (factory reset -> re-pair), and a long hold is
     * NOT a single-click, so the two don't collide. */
    iot_button_register_cb(handle, BUTTON_SINGLE_CLICK, NULL, app_driver_button_toggle_cb, NULL);

    /* Hold BOOT ~15 s (10 s + a 5 s yellow-flash confirm) to factory-reset and
     * re-pair; releasing before 15 s cancels. PRESS_DOWN starts the hold timer,
     * PRESS_UP cancels/restores. A short click still toggles the bulb. */
    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, reset_press_down_cb, NULL);
    iot_button_register_cb(handle, BUTTON_PRESS_UP,   NULL, reset_press_up_cb,   NULL);
    return (app_driver_handle_t)handle;
}

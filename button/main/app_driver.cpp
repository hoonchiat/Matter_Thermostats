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

#include <esp_timer.h>

#include <device.h>
#include <led_driver.h>
#include <button_gpio.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";

/* Onboard RGB LED - used only as the pairing-mode indicator. */
static led_driver_handle_t s_led = NULL;
/* The Generic Switch endpoint the button raises events on. */
static uint16_t s_switch_ep = 1;

/* ---- pairing-mode slow flash (blue) ------------------------------------- *
 * While a commissioning window is open the LED blinks blue ~0.7 s on/off so
 * it's obvious the device is waiting to pair; LED off when the window closes. */
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
        if (s_led) led_driver_set_power(s_led, false);
        ESP_LOGI(TAG, "Pairing ended");
    }
}

/* ---- per-press LED feedback -------------------------------------------- *
 * When NOT pairing the LED is off; each button gesture flashes a distinct
 * colour for ~0.5 s so the user sees which action was performed:
 *   single = green, double = blue, long = red. */
static esp_timer_handle_t s_flash_timer = NULL;

static void flash_off_cb(void *)
{
    if (s_led && !s_pairing) led_driver_set_power(s_led, false);
}

static void flash_gesture(uint16_t hue)
{
    if (s_pairing || !s_led) return;     /* don't fight the pairing slow-flash */
    if (!s_flash_timer) {
        const esp_timer_create_args_t args = { .callback = flash_off_cb, .name = "gestflash" };
        esp_timer_create(&args, &s_flash_timer);
    }
    esp_timer_stop(s_flash_timer);       /* restart the window if pressed again */
    led_driver_set_hue(s_led, hue);
    led_driver_set_saturation(s_led, 100);
    led_driver_set_brightness(s_led, 40);
    led_driver_set_power(s_led, true);
    esp_timer_start_once(s_flash_timer, 500 * 1000);   /* 0.5 s */
}

/* ---- BOOT button -> Matter Switch (Generic Switch, cluster 0x003B) events ----
 * The Switch event senders touch the data model, so they run on the Matter task
 * via ScheduleWork. Position 1 = pressed, 0 = released. The three button
 * functions map to distinguishable momentary-switch event sequences:
 *   single press -> InitialPress, ShortRelease, MultiPressComplete(count=1)
 *   double press -> InitialPress, ShortRelease, MultiPressComplete(count=2)
 *   long  press  -> InitialPress, LongPress ... (on release) LongRelease         */
enum btn_gesture { GEST_SINGLE = 1, GEST_DOUBLE, GEST_LONG_START, GEST_LONG_UP };

static void switch_event_work(intptr_t g)
{
    using namespace esp_matter::cluster::switch_cluster::event;
    uint16_t ep = s_switch_ep;
    switch ((btn_gesture)g) {
    case GEST_SINGLE:
        send_initial_press(ep, 1);
        send_short_release(ep, 1);
        send_multi_press_complete(ep, 1, 1);
        ESP_LOGI(TAG, "Button: SINGLE press -> InitialPress/ShortRelease/MultiPressComplete(1)");
        break;
    case GEST_DOUBLE:
        send_initial_press(ep, 1);
        send_short_release(ep, 1);
        send_multi_press_complete(ep, 1, 2);
        ESP_LOGI(TAG, "Button: DOUBLE press -> MultiPressComplete(2)");
        break;
    case GEST_LONG_START:
        send_initial_press(ep, 1);
        send_long_press(ep, 1);
        ESP_LOGI(TAG, "Button: LONG press -> InitialPress/LongPress");
        break;
    case GEST_LONG_UP:
        send_long_release(ep, 1);
        ESP_LOGI(TAG, "Button: LONG release -> LongRelease");
        break;
    }
}

static void btn_gesture_cb(void *arg, void *data)
{
    /* LED feedback so the user sees which action fired (off otherwise). */
    switch ((btn_gesture)(intptr_t)data) {
    case GEST_SINGLE:     flash_gesture(120); break;   /* green */
    case GEST_DOUBLE:     flash_gesture(240); break;   /* blue  */
    case GEST_LONG_START: flash_gesture(0);   break;   /* red   */
    default: break;                                    /* LONG_UP: no extra flash */
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(switch_event_work, (intptr_t)data);
}

app_driver_handle_t app_driver_led_init()
{
    led_driver_config_t config = led_driver_get_config();
    led_driver_handle_t handle = led_driver_init(&config);
    s_led = handle;
    return (app_driver_handle_t)handle;
}

app_driver_handle_t app_driver_button_init(uint16_t switch_endpoint_id)
{
    s_switch_ep = switch_endpoint_id;

    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();
    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    /* Three button functions -> Matter Switch events (usr_data carries the gesture).
     * Long-press threshold = CONFIG_BUTTON_LONG_PRESS_TIME_MS (set to 1000 ms). */
    iot_button_register_cb(handle, BUTTON_SINGLE_CLICK,     NULL, btn_gesture_cb, (void *)GEST_SINGLE);
    iot_button_register_cb(handle, BUTTON_DOUBLE_CLICK,     NULL, btn_gesture_cb, (void *)GEST_DOUBLE);
    iot_button_register_cb(handle, BUTTON_LONG_PRESS_START, NULL, btn_gesture_cb, (void *)GEST_LONG_START);
    iot_button_register_cb(handle, BUTTON_LONG_PRESS_UP,    NULL, btn_gesture_cb, (void *)GEST_LONG_UP);
    return (app_driver_handle_t)handle;
}

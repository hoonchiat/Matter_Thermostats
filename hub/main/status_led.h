/*
 * status_led.h - onboard addressable RGB LED (WS2812 on GPIO8 of the
 * ESP32-C6-DevKitC-1) used as a hub status indicator.
 *
 *   LED_BOOTING   - solid blue    (starting up / Thread forming)
 *   LED_OPERATING - solid green   (ready, running, subscribed)
 *   LED_PAIRING   - blinking amber (commissioning mode, discoverable)
 *
 * Driven from a single 250 ms esp_timer (blink + refresh) via the RMT TX
 * peripheral - no extra task. If the LED GPIO differs on your board, change
 * LED_GPIO in status_led.cpp.
 */
#pragma once

enum led_state_t { LED_BOOTING = 0, LED_OPERATING, LED_PAIRING };

void status_led_init(void);
void status_led_set_state(led_state_t st);

/*
 * occupancy.h - local occupancy auto-off for the RGB light.
 *
 * A PIR / occupancy sensor is wired to a GPIO on the C6. If no presence is seen
 * for a configurable period (default 10 min) the light turns itself OFF. This is
 * entirely local - it does NOT depend on the hub. The hub only *configures* the
 * period, over Matter (a custom writable attribute on the OnOff cluster).
 *
 * Graceful degradation: the input uses an internal pull-up and presence is
 * active-HIGH, so with NO sensor wired the pin reads "present" forever - the
 * timer never expires and the light behaves normally.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Custom manufacturer attribute (VID 0xFFF1) on the OnOff cluster that the hub
 * writes to set the auto-off period, in MINUTES. */
#define OCC_TIMEOUT_ATTR_ID 0xFFF10000u

/* Start the occupancy input + auto-off task. `light_endpoint_id` is the light's
 * OnOff endpoint. Loads the persisted period (default 10 min). Call once, after
 * esp_matter::start(). */
void occupancy_start(uint16_t light_endpoint_id);

/* Set the auto-off period in minutes (clamped 1..1440), persisted to NVS. Called
 * from the Matter attribute-write callback when the hub writes OCC_TIMEOUT_ATTR_ID. */
void occupancy_set_timeout_min(uint16_t minutes);

uint16_t occupancy_get_timeout_min(void);

#ifdef __cplusplus
}
#endif

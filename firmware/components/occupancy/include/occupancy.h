/*
 * occupancy — Home/Away presence detection.
 *
 * Two sources, selectable at runtime:
 *   - a hardware occupancy/PIR sensor (a GPIO that goes active on motion), with
 *     a configurable "vacancy timeout" before the room is considered unoccupied;
 *   - a manual Home/Away toggle (settings menu / app state) when no sensor is
 *     wired or the user prefers manual control.
 *
 * The vacancy-timeout logic is a pure function (occupancy_from_motion) so it is
 * unit-testable on a host; the GPIO read lives in the ESP driver.
 */
#ifndef OCCUPANCY_H
#define OCCUPANCY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  gpio;               /* occupancy/PIR input; < 0 = no sensor wired */
    int  vacancy_timeout_s;  /* no motion for this long => unoccupied       */
    bool active_high;        /* true if the sensor drives the pin high on motion */
} occupancy_config_t;

/* Configure the input GPIO (no-op if gpio < 0). Returns ESP_OK. */
int  occupancy_init(const occupancy_config_t *cfg);

/* Whether a hardware sensor is configured. */
bool occupancy_sensor_present(void);

/*
 * Sample the sensor and return the current occupied state (true = occupied).
 * With no sensor present this returns true (fail-safe to "occupied", i.e. comfort).
 */
bool occupancy_poll(uint64_t now_ms);

/* PURE: occupied while the time since the last motion is under the timeout. */
bool occupancy_from_motion(uint64_t now_ms, uint64_t last_motion_ms, uint32_t timeout_s);

#ifdef __cplusplus
}
#endif

#endif /* OCCUPANCY_H */

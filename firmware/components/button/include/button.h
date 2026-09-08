/*
 * button — debounced momentary buttons with short/long-press detection.
 * A single esp_timer polls all registered buttons and posts typed events to a
 * FreeRTOS queue supplied by the caller. Used for the encoder switch, the
 * dedicated push button, and the BOOT/factory-reset button.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_EVENT_SHORT = 0,   /* pressed and released before the long threshold */
    BUTTON_EVENT_LONG,        /* held past the long threshold (fires once)      */
} button_event_type_t;

typedef struct {
    int                 id;   /* caller-defined identifier (echoed in events) */
    button_event_type_t type;
} button_event_t;

typedef struct {
    int      gpio;
    int      id;
    bool     active_low;      /* true for pull-up + switch-to-GND wiring */
    uint32_t long_press_ms;   /* e.g. 3000 for menu, 5000 for factory reset */
} button_cfg_t;

/* Create the poll timer and the event pathway. Call once. */
int button_init(QueueHandle_t event_queue);

/* Register a button; safe to call several times (up to the internal max). */
int button_add(const button_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */

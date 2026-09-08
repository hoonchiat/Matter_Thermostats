/*
 * relays — HVAC output driver (W / Y / G / O·B). Thin GPIO layer that applies
 * the desired output state atomically and fails safe (all off) on init.
 */
#ifndef RELAYS_H
#define RELAYS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  gpio_w;        /* heat        */
    int  gpio_y;        /* cool/compressor */
    int  gpio_g;        /* fan         */
    int  gpio_ob;       /* reversing valve (heat pump); set < 0 to disable */
    bool active_high;   /* true if a logic-1 energizes the load */
} relays_config_t;

typedef struct {
    bool w;
    bool y;
    bool g;
    bool ob;
} relays_state_t;

/* Configure the output GPIOs and drive everything OFF. */
int  relays_init(const relays_config_t *cfg);

/* Apply a full output state. */
void relays_apply(const relays_state_t *st);

/* Force all outputs off (fault / safety). */
void relays_all_off(void);

#ifdef __cplusplus
}
#endif

#endif /* RELAYS_H */

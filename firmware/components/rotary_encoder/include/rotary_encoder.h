/*
 * rotary_encoder — quadrature decoding via the ESP32 PCNT peripheral.
 * Counts A/B transitions in hardware (with a glitch filter) so no detents are
 * lost, and reports whole-detent deltas to the caller.
 */
#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio_a;             /* encoder A / CLK */
    int gpio_b;             /* encoder B / DT  */
    int counts_per_detent;  /* pulses per mechanical click (EC11: usually 4) */
    int glitch_ns;          /* PCNT glitch filter, e.g. 1000 ns */
    bool invert;            /* swap direction if CW reads negative */
} rotary_encoder_config_t;

/* Initialize the PCNT unit + channels and start counting. */
int rotary_encoder_init(const rotary_encoder_config_t *cfg);

/*
 * Return the number of *detents* moved since the last call (signed; CW positive
 * unless inverted). Sub-detent motion is retained internally and applied later.
 */
int rotary_encoder_get_delta(void);

#ifdef __cplusplus
}
#endif

#endif /* ROTARY_ENCODER_H */

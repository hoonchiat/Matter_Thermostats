/*
 * Per-device commissioning identity (discriminator / passcode / serial) read from
 * the `fctry` flash partition, written there per-unit by the HTML provisioning tool.
 * Registers a custom Matter CommissionableDataProvider so every board runs the SAME
 * app image but commissions with its own discriminator + passcode. The DAC stays the
 * built-in test DAC (attestation still passes on the hub).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load provisioning from the `fctry` partition and register the provider.
 *  Call ONCE, BEFORE esp_matter::start().
 *  @param default_serial  serial used if the partition is blank/unprovisioned. */
void        provisioning_init(const char *default_serial);

const char *provisioning_serial(void);          /* resolved serial (partition or default) */
uint16_t    provisioning_discriminator(void);   /* resolved discriminator                 */
bool        provisioning_is_provisioned(void);  /* true if valid data was in the partition */

#ifdef __cplusplus
}
#endif

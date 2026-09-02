/*
 * cfg.h - persisted portal configuration (NVS-backed).
 *
 * Editable from the SPA's Configuration tab via the S3-local WS commands cfg_get /
 * cfg_set / s3_reboot (handled by the web worker, NOT forwarded to the C6). tz and
 * poll apply live; network fields (Wi-Fi / AP / hostname / NTP) apply on reboot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  poll_s;         /* status poll interval, 1..60 s              */
    int  tz_min;         /* local = UTC + tz_min; -720..840            */
    char ntp[64];        /* SNTP server                                */
    char hostname[32];   /* mDNS / DHCP hostname (-> <hostname>.local)  */
    char ap_ssid[33];    /* SoftAP SSID; "" = auto "MatterGateway-XXXX" */
    char ap_pass[65];    /* SoftAP WPA2 password (>=8)                  */
    char sta_ssid[33];   /* Wi-Fi STA SSID to join                     */
    char sta_pass[65];   /* Wi-Fi STA password                         */
} portal_cfg_t;

void               cfg_load(void);           /* load from NVS (defaults if absent) */
void               cfg_save(void);           /* persist current config             */
const portal_cfg_t *cfg_get(void);           /* current (read-only)                */

/* Current config as JSON (caller frees). Passwords are returned MASKED (empty). */
cJSON *cfg_to_json(void);

/* Apply a cfg_set request object: updates + saves. *reboot set if a network field
 * changed (needs a reboot to take effect); *tz_changed set if tz changed (caller
 * re-pushes tz/time). Returns false + err on a validation failure. */
bool cfg_apply_json(const cJSON *req, char *err, size_t cap, bool *reboot, bool *tz_changed);

#ifdef __cplusplus
}
#endif

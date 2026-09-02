/*
 * net.h - Wi-Fi STA + mDNS + SNTP for the portal.
 *
 * Connects to the office Wi-Fi, advertises esp32.local, and keeps the system clock
 * synced from NTP. tz offset (default GMT+8) is applied by higher layers when they
 * build local-time strings for the C6.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up Wi-Fi STA, mDNS (esp32.local), and SNTP. Call once (after nvs_flash_init). */
void net_start(void);

bool        net_wifi_connected(void);   /* STA got an IP */
bool        net_time_valid(void);        /* SNTP has synced at least once */
const char *net_ip_str(void);            /* STA IP: "192.168.x.y" or "0.0.0.0" */
int         net_tz_offset_min(void);     /* local = UTC + this (from cfg; GMT+8 default) */
const char *net_ap_ssid(void);           /* own SoftAP SSID (portal at 192.168.4.1) */

#ifdef __cplusplus
}
#endif

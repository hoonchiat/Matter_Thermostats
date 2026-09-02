/*
 * portal.h - the portal "application" layer.
 *
 * Owns the periodic poll of the C6 (state cache) and the NTP->hub->devices time-sync.
 * Sits above usb_bridge (transport) and net (Wi-Fi/NTP). Phase 3's WebSocket layer
 * reads the cached state and pushes it to browsers.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the poll/time-sync app task. Call after usb_bridge_start() + net_start(). */
void portal_start(void);

/* Copy the last cached `status` response line (NUL-terminated) into out.
 * Returns the length copied, or 0 if nothing cached yet. Thread-safe. */
size_t portal_get_status(char *out, size_t len);

/* Force a time re-sync (settime + devtime) on the next poll, e.g. after tz change. */
void portal_resync(void);

/* Air-quality history for the Charts tab (dev=0 = all air sensors). Caller frees. */
struct cJSON *portal_history_json(int dev);

#ifdef __cplusplus
}
#endif

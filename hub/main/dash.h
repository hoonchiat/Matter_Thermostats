/*
 * dash.h - one-shot live "dashboard" overview of every paired device (v1.5).
 *
 * `dash` on the console live-reads each device in turn (sequentially, so the
 * heap-tight radio never has several reads in flight at once) and prints a single
 * snapshot table: present VALUES (air-quality sensors, plug on/off, battery) plus
 * STATUS (online/stale/offline, last-seen age, RSSI/LQI, enumerated, last button
 * press). A per-device timeout keeps a sleepy/offline device (e.g. the BILRESA)
 * from stalling the sweep - it is simply shown unreachable.
 *
 * Additive: no change to the commissioning path. Values are read on demand, not
 * from a cache (the user asked for live freshness).
 */
#pragma once

#include <cstdint>

/* Console dispatch: handles `dash` / `dash help`. Returns true if handled. */
bool dash_handle_cmd(const char *line);

/* One-line help, appended to the console help. */
void dash_print_help(void);

/* Record a device event so `dash` can show the last button press. Call from the
 * Matter event-report callback (CHIP task). Cheap; only Switch events are kept. */
void dash_on_event(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t event_id);

/* Subscription paths dash contributes (battery), so a sleepy device's battery is
 * cached passively from its own reports - the fallback shown when a live `dash`
 * read misses it. Consumed by the subscription builder in main.cpp, mirroring
 * logic_sensor_path*. */
int  dash_sub_path_count(void);
void dash_sub_path(int i, uint32_t *cluster, uint32_t *attr);

/* Ingest an attribute report (called from the subscription attribute callback,
 * CHIP task). Caches BatPercentRemaining for the offline fallback; ignores the
 * rest. */
void dash_on_report(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attr_id, double value);

/* Last-known battery for a slot (from the passive subscription cache). Returns
 * false if none cached. `pct` = percent (0..100), `age_s` = seconds since cached.
 * Used by the USB JSON protocol's `status` builder. */
bool dash_get_battery(int slot, int *pct, int *age_s);

/* Last-known OnOff for a slot (from the passive subscription cache). Returns false
 * if none cached. Used by the USB JSON protocol's `status` builder. */
bool dash_get_onoff(int slot, bool *on);

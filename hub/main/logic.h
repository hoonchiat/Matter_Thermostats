/*
 * logic.h - user-defined, value-triggered automation rules for the Matter Hub
 *           (v1.5).
 *
 * Where bindings.cpp relays a device EVENT (button press) to an OnOff target,
 * this module acts on device VALUES: a rule watches a sensor (temperature, CO2,
 * humidity, PM...) on one or more source devices, optionally aggregates them
 * (avg/min/max), compares against a threshold with a hysteresis deadband, and
 * drives an OnOff target when the condition crosses.
 *
 *   e.g.  temp #1 > 28        -> ON  plug #4   (fan follows the sensor)
 *         co2 avg #1 #2 > 1250 -> ON plug #5
 *
 * Two mechanics for how sensor values reach the hub and how rules act:
 *  - Values arrive by SUBSCRIPTION. Every device is subscribed to a small fixed
 *    set of well-known measurement clusters (wildcard endpoint), so a rule needs
 *    no re-subscribe to take effect - it just reads the latest cached value.
 *    logic_sensor_path_count()/logic_sensor_path() expose that set to the
 *    subscription builder in main.cpp; logic_on_report() ingests each value.
 *  - Each rule is TRACK (auto-inverse when the condition clears) or ONESHOT
 *    (fire once on the rising edge).
 *
 * Additive: no change to the commissioning path (FOUNDATION §5/§11). This header
 * is deliberately free of any CHIP/esp-matter type so it can be included cheaply.
 */
#pragma once

#include <cstdint>
#include <cstddef>

struct cJSON;
struct restore_map;

/* Console dispatch: handles logic / logic add / logic rm / logic help.
 * Returns true if the line was a logic command (handled), false otherwise. */
bool logic_handle_cmd(const char *line);

/* --- config backup/restore ------------------------------------------------- */
/* Serialise the logic rule table to a cJSON array (device refs as backup indices
 * via slot2idx[slot]). Caller owns the returned node. */
cJSON *logic_to_json(const int *slot2idx);
/* Rebuild the logic rule table from a backup array, resolving device indices to
 * current slots via `m`. A rule referencing an unresolved device is skipped and
 * reported. Writes NVS + reloads. Returns counts via out params. */
void logic_apply_json(const cJSON *arr, const restore_map *m, int *applied, int *skipped);

/* Load the persisted rule table from NVS. Call once at boot (before subscribe). */
void logic_load(void);

/* --- sensor cache accessors (for the USB JSON `status` builder) ------------- */
int         logic_num_sensors(void);            /* count of tracked measurements */
const char *logic_sensor_name(int i);           /* "temp"/"humidity"/"co2"/"pm25" */
const char *logic_sensor_unit(int i);           /* "C"/"%"/"ppm"/"ug/m3"         */
bool        logic_reading(int slot, int i, float *val);   /* latest cached value  */

/* --- structured entry points (shared by the console + the JSON protocol) ---- */
/* Add a rule from a console-syntax expression (everything after "logic add").
 * Returns true on success; on failure fills `err` (may be null). */
bool logic_add_expr(const char *expr, char *err, size_t errcap);
bool logic_rm_idx(int idx, char *err, size_t errcap);
/* Rules as a JSON array of {idx, text, state} for a `logic_list` query. */
struct cJSON *logic_list_json(void);

/* Arm the periodic safety re-evaluation timer. Call once after Matter is up. */
void logic_start(void);

/* One-line-per-command help, appended to the console help. */
void logic_print_help(void);

/* --- subscription integration (called from main.cpp's subscribe builder) --- */
/* Number of measurement attribute paths every device should subscribe to. */
int  logic_sensor_path_count(void);
/* Fill (cluster,attr) for sensor path i (0..count-1). Endpoint is wildcard. */
void logic_sensor_path(int i, uint32_t *cluster, uint32_t *attr);

/* Ingest one attribute report (called from the subscription attribute callback,
 * on the CHIP task). Non-sensor paths are ignored. A matching value updates the
 * cache and re-evaluates any rule that references it - which may originate an
 * OnOff command (safe: we are already on the CHIP task). `value` is the raw
 * numeric value from TLV; this module applies the per-sensor scale. */
void logic_on_report(uint64_t node_id, uint16_t endpoint_id,
                     uint32_t cluster_id, uint32_t attr_id, double value);

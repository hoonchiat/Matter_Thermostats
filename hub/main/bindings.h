/*
 * bindings.h - device-to-device binding for the Matter Hub (v1.4).
 *
 * Two mechanisms, auto-selected per source device (Path C / hybrid):
 *
 *  1. NATIVE Matter binding - for a source that exposes the Binding cluster
 *     (0x001E) server (e.g. a proper OnOffLightSwitch). The hub writes the
 *     source's Binding table AND adds an ACL entry on the target so the source
 *     may command it directly, hub-independent. (Read-modify-write on both, so
 *     the target's admin ACL entry is never clobbered.)
 *
 *  2. HUB-MEDIATED rule - for an event-only Generic Switch (0x003B) that cannot
 *     hold a binding table (e.g. IKEA BILRESA). The hub subscribes to its button
 *     events (it already does) and relays the action (OnOff on/off/toggle) to the
 *     target. Persisted in NVS. Works with the hardware we have today.
 *
 * `bindable` classifies each device so the user can see which applies. `bind`
 * picks the right mechanism automatically.
 */
#pragma once

#include <cstdint>
#include <cstddef>

struct cJSON;
struct restore_map;

/* Console dispatch: handles bind / bindings / unbind / bindable.
 * Returns true if the line was a bindings command (handled), false otherwise. */
bool bindings_handle_cmd(const char *line);

/* --- config backup/restore ------------------------------------------------- */
/* Serialise the hub-mediated rule table to a cJSON array (device refs as backup
 * indices via slot2idx[slot]). Rules whose src/dst device isn't currently known
 * are skipped. Caller owns the returned node. */
cJSON *bindings_to_json(const int *slot2idx);
/* Rebuild the hub rule table from a backup array, resolving device indices to
 * current slots via `m`; a rule referencing an unresolved device is skipped and
 * reported. Writes NVS. Returns counts via out params. */
void bindings_apply_json(const cJSON *arr, const restore_map *m, int *applied, int *skipped);

/* Load the persisted hub-mediated rule table from NVS. Call once at boot. */
void bindings_load(void);

/* --- structured entry points (shared by the console + the JSON protocol) ----
 * src/dst are 1-based device numbers; dst_ep<=0 means auto-find the OnOff ep;
 * action is ACT_OFF/ACT_ON/ACT_TOGGLE (0/1/2). Native binds are async ("sent").
 * Returns true on success; on failure fills `err` (may be null). */
bool bindings_add(int src, int src_ep, int dst, int dst_ep, uint8_t action, const char *press, char *err, size_t errcap);
bool bindings_unbind_idx(int idx, char *err, size_t errcap);
/* Hub-mediated rules as a JSON array of {idx,src,src_ep,dst,dst_ep,action}. */
struct cJSON *bindings_list_json(void);
/* Per-device bindable endpoints for building a binding UI:
 * [{slot,name,enumerated,eps:[{ep, src?:"native"|"switch", target?:true}]}].
 * Only devices with at least one bindable endpoint are listed. */
struct cJSON *bindings_bindable_json(void);

/* Dispatch a device event through the hub-mediated rule table. Call from the
 * Matter event-report callback (runs on the CHIP task). If any rule matches
 * (src node + endpoint + trigger event), the mapped action is sent to the
 * target. Cheap no-op when no rule matches. */
void bindings_on_event(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t event_id, uint8_t count);

/* One-line-per-command help, appended to the console help. */
void bindings_print_help(void);

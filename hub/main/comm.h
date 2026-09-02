/*
 * comm.h - JSON request/response protocol over the native USB-Serial-JTAG port
 *          (v1.8, COM7). Separate from the UART0 console + logs.
 *
 * Framing: NDJSON - one COMPACT JSON object per line, '\n'-terminated, both ways.
 *   request:  {"id":<int>,"cmd":"<name>", ...}
 *   ok:       {"id":<int>,"ok":true,"result":{...}}
 *   error:    {"id":<int>,"ok":false,"error":"<msg>"}
 *
 * Every command is typed JSON in / structured JSON out. Command execution is
 * serialised with the UART console via a shared command mutex (both mutate the
 * device table / rule tables). Additive: commissioning path untouched.
 */
#pragma once

#include <cstddef>

struct cJSON;

/* Install the USB-Serial-JTAG driver and start comm_task. Call once at boot,
 * after the config modules (nvs/bindings/logic/schedule) have loaded. */
void comm_start(void);

/* ---- protocol support implemented in main.cpp (owns the shared globals) ----- */
/* Serialise command execution with the UART console. */
void hub_cmd_lock(void);
void hub_cmd_unlock(void);
/* Build the whole config as a cJSON object (caller owns it). */
cJSON *hub_backup_json(void);
/* Stage a restore from a backup object (reboots on success; may re-pair). */
bool hub_stage_restore(const cJSON *backup, bool wipe, char *err, size_t errcap);
/* A restore is "pending" while device-referencing rules await their devices to
 * re-commission; cancel drops the stashed backup so it stops re-applying. */
bool hub_restore_pending(void);
void hub_restore_cancel(void);
/* Structured handler for the NON-rebooting global commands: name, payload, pin,
 * debug, settime. Fills `result` and/or `err`. Returns true on success. */
bool hub_action_json(const char *cmd, const cJSON *req, cJSON *result, char *err, size_t errcap);

/* Rebooting actions (the caller must send its JSON response BEFORE calling these,
 * since they esp_restart). `payloads` is a cJSON array of strings. */
bool hub_stage_pair(const char *payload, char *err, size_t errcap);       /* reboots */
bool hub_stage_pairlist(const cJSON *payloads, int *queued, char *err, size_t errcap); /* reboots */
bool hub_remove_device(int dev, char *err, size_t errcap);                /* reboots */
void hub_factory_reset(void);                                            /* reboots */

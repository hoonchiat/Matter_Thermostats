/*
 * inspector.h - generic Matter device inspector (enumeration + console).
 *
 * Additive layer on top of the controller core: it never touches commissioning.
 * On CASE-up it walks the device (wildcard attribute read) and caches the
 * endpoint/cluster/attribute tree in the DeviceInfo model; the console commands
 * render that tree and issue live read/write/invoke interactions.
 */
#pragma once

#include <cstdint>

/* Called from the subscribe-done callback (operating mode, CASE up) to build /
 * refresh a device's cached tree. No-op if already enumerated (use 'scan' to
 * force). Safe to call from any task - it schedules work on the CHIP loop. */
void inspector_on_case_up(uint64_t node_id);

/* Handle an inspector console command (devices/device/tree/ep/cluster/read/
 * scan/write/invoke). Returns true if the line was consumed, false if it is not
 * an inspector command (so the core dispatcher can try it). */
bool inspector_handle_cmd(const char *line);

/* One-line-per-device summary. Exposed so the core 'status' command can reuse it. */
void inspector_print_devices(void);

/* Multi-line help for the inspector commands (appended to the core help). */
void inspector_print_help(void);

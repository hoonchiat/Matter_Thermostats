/*
 * usb_bridge.h - USB-host CDC-ACM bridge to the ESP32-C6 Matter hub.
 *
 * The S3 is the USB host; the C6's native USB (USB-Serial-JTAG, VID 0x303A/PID 0x1001)
 * is the device. We open its CDC-ACM interface and speak the C6's NDJSON request/
 * response protocol (one compact JSON object per line, '\n'-terminated).
 *
 * Concurrency model (Phase 1): one request outstanding at a time. usb_bridge_request()
 * injects an "id", sends the line, and blocks until the response line with the matching
 * id arrives (or a timeout). Higher layers (the WS relay) serialise through this.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    C6_OFFLINE = 0,      /* no CDC device open */
    C6_ONLINE,           /* CDC open, JSON answering */
    C6_COMMISSIONING,    /* a rebooting command was sent / CDC dropped mid-commission */
} c6_link_state_t;

/* Start the USB host + CDC bridge. Spawns the USB lib task, the CDC driver task,
 * an RX line-dispatch task, and the bridge (open/reconnect) task. Call once. */
void usb_bridge_start(void);

/* Current link state (cheap, lock-free read). */
c6_link_state_t usb_bridge_link_state(void);
const char     *usb_bridge_link_name(void);

/* Send a request to the C6 and wait for the matching response.
 *  - `cmd_json`  : a JSON object string WITHOUT an id (e.g. {"cmd":"status"}); the
 *                  bridge injects a fresh "id".
 *  - `resp`/`resp_len` : receives the raw response line (NUL-terminated, no newline).
 *  - `timeout_ms`: bound on the wait.
 * Returns true iff a matching response arrived within the timeout. Thread-safe;
 * requests are serialised (one outstanding on the C6 at a time). */
bool usb_bridge_request(const char *cmd_json, char *resp, size_t resp_len, int timeout_ms);

#ifdef __cplusplus
}
#endif

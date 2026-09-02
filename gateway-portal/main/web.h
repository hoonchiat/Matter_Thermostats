/*
 * web.h - HTTP server + WebSocket relay for the browser SPA.
 *
 * Serves the single-page app and a /ws WebSocket. Browsers send {cmd,...,tag}; the
 * S3 relays to the C6 (via a worker task, so the HTTP server never blocks on USB) and
 * returns {type:"reply",tag,...}. The S3 also pushes {type:"state"} (device snapshot)
 * and {type:"link"} (hub link) unsolicited.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server + WebSocket. Call after net_start()/portal_start(). */
void web_start(void);

/* Broadcast the cached device snapshot ({type:"state"}) to all WS clients.
 * Called by the portal after each poll. No-op if no clients are connected. */
void web_push_state(void);

/* Broadcast hub link state ({type:"link"}) to all WS clients. Called on change. */
void web_push_link(void);

#ifdef __cplusplus
}
#endif

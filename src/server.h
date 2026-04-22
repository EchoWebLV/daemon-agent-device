// ---------------------------------------------------------------------------
//  Tiny HTTP server that serves the phone UI and the /say API.
//
//  The board has no mic, so the user talks from their phone: open
//  http://<board-ip>/ in Chrome, tap the mic, and the browser does the STT
//  locally (Web Speech API) before POSTing the transcript to /say.
//
//  The AI module registers a `say` handler here; it receives the user text
//  and fills a reply buffer (which is returned to the caller as JSON).
//  That handler is allowed to block — the HTTP task will wait for it — so
//  a Gemini round-trip + TTS kick-off happens inline before the phone sees
//  the reply.
//
//  Wi-Fi is *not* this module's business; it's in wifi_sta.h. Callers
//  must have Wi-Fi up before calling server_start().
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Signature for the "user said something" handler. `reply_out` is a
// caller-owned buffer of `reply_cap` bytes (including null terminator).
// The handler writes a UTF-8 reply string into it; the server JSON-encodes
// and sends it. On empty reply, the server returns {"reply":""}.
typedef void (*server_say_handler_t)(const char *user,
                                     char *reply_out, size_t reply_cap);

// Start the HTTP server on port 80. Safe to call more than once; subsequent
// calls are no-ops. Returns ESP_OK on success.
esp_err_t server_start(void);

// Stop the server (rarely needed outside tests).
void server_stop(void);

// Register the /say handler. Replaces any previous handler; pass NULL to
// clear. Thread-safe — takes effect on the next request.
void server_set_say_handler(server_say_handler_t h);

// Update the short status string reported on /state. Copied internally so
// the caller's buffer doesn't have to outlive the call.
void server_set_status(const char *status);

#ifdef __cplusplus
}
#endif

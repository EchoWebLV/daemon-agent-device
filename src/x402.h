// ---------------------------------------------------------------------------
//  x402 HTTP client.
//
//  Handles the "402 Payment Required → sign a Solana USDC transfer →
//  retry with a PAYMENT-SIGNATURE header" dance. Mirrors the Chrome
//  extension's handlePaymentAndRetry behaviour.
//
//  Caller-allocates the body buffer. We never block on a response that
//  exceeds it — the handler stops writing once full and subsequent parses
//  work on whatever arrived first.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    status;       // final HTTP status (after payment if one was required)
    double cost_usd;     // USDC charged this call (0 on free / failure)
    char   error[96];    // "" on success, short human-readable msg otherwise
    size_t body_len;     // bytes of response body actually captured
} x402_result_t;

// Issue an HTTP request to `url` with `method` ("GET", "POST", "PUT",
// "PATCH", "DELETE" — case-sensitive, NULL treated as "POST"). If the first
// response is 402, build + sign a Solana USDC payment per the returned
// payment-required description and retry with a PAYMENT-SIGNATURE header.
// Writes the final response body into `body_buf` (NUL-terminated, truncated
// on overflow).
//
// `json_body` is ignored for methods that don't carry a body (GET, DELETE);
// pass NULL in that case. `auth_bearer`, if non-NULL and non-empty, is sent
// as "Authorization: Bearer <value>".
void x402_call(const char *method,
               const char *url,
               const char *json_body,
               const char *auth_bearer,
               char       *body_buf,
               size_t      body_cap,
               x402_result_t *out);

// Back-compat shim — equivalent to x402_call("POST", ...).
void x402_post(const char *url,
               const char *json_body,
               const char *auth_bearer,
               char       *body_buf,
               size_t      body_cap,
               x402_result_t *out);

// Streaming variant. Invoked once per chunk of response body as it arrives;
// return true to keep streaming, false to abort. Total bytes seen is
// reported via x402_result_t.body_len. The 402 + payment-retry dance
// happens internally exactly like x402_call(); only the body delivery
// differs (callback per chunk, no buffer).
typedef bool (*x402_chunk_cb_t)(const char *data, size_t len, void *user);

void x402_call_stream(const char *method,
                      const char *url,
                      const char *json_body,
                      const char *auth_bearer,
                      x402_chunk_cb_t on_chunk,
                      void          *cb_user,
                      x402_result_t *out);

#ifdef __cplusplus
}
#endif

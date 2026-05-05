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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      status;       // final HTTP status (after payment if one was required)
    double   cost_usd;     // USDC charged this call (0 on free / failure)
    uint64_t cost_micros;  // exact integer base-units from the payment envelope (0 on free / failure)
    char     error[96];    // "" on success, short human-readable msg otherwise
    size_t   body_len;     // bytes of response body actually captured
} x402_result_t;

// Guard hook: called after the 402 payment amount is known but before the
// Solana transaction is signed and submitted. Allows the caller to inspect
// the cost and approve, deny, or refuse the payment.
typedef enum {
    X402_GUARD_AUTO,            // proceed with payment (default / no-op)
    X402_GUARD_CONFIRM_OK,      // explicitly confirmed by caller — proceed
    X402_GUARD_CONFIRM_DENIED,  // user declined — abort with "user_declined"
    X402_GUARD_REFUSE,          // policy refused — abort with "policy_refused"
} x402_guard_decision_t;

typedef x402_guard_decision_t (*x402_guard_cb_t)(
    uint64_t actual_micros, const char *description, void *user);

// Issue an HTTP request with an optional guard hook that fires between
// receiving the 402 and signing the payment. `guard` may be NULL (behaves
// identically to x402_call). `guard_user` is forwarded to the callback.
// See x402_call for parameter semantics.
void x402_call_with_guard(const char *method, const char *url,
                          const char *json_body, const char *auth_bearer,
                          char *body_buf, size_t body_cap,
                          x402_guard_cb_t guard, void *guard_user,
                          x402_result_t *out);

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

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

// POST `json_body` to `url`. If the first response is 402, build and sign
// a Solana USDC payment per the returned payment-required description and
// retry. Writes the final response body into `body_buf` (NUL-terminated,
// truncated on overflow).
//
// `auth_bearer`, if non-NULL and non-empty, is sent as
// "Authorization: Bearer <value>".
void x402_post(const char *url,
               const char *json_body,
               const char *auth_bearer,
               char       *body_buf,
               size_t      body_cap,
               x402_result_t *out);

#ifdef __cplusplus
}
#endif

// ============================================================================
//  x402 HTTP client.
//
//  Wraps a request/response cycle where the server may respond 402 Payment
//  Required with a JSON payment-required description. The client then
//  constructs a signed Solana USDC transfer using `solana_tx`, base64-
//  wraps it in the x402 payment payload, and retries the original request
//  with a `PAYMENT-SIGNATURE` header.
//
//  Mirrors the JS flow from the Daemon Chrome extension
//  (src/lib/x402-client.ts → handlePaymentAndRetry).
// ============================================================================
#pragma once
#include <Arduino.h>

// Result: body + HTTP status of the final response (after payment, if one
// was required).
struct X402Result {
  int    status    = 0;
  String body;
  double costUsd   = 0.0;   // actual USDC charged this call (0 if free / failed)
  String error;             // non-empty on failure
};

// POST `jsonBody` to `url`. If the first response is 402, pay and retry
// automatically. The response body goes into `out.body`.
//
// `apiKey`, if non-empty, is sent as Authorization: Bearer.
X402Result x402Post(const String &url,
                    const String &jsonBody,
                    const String &authBearer = String());

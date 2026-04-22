// ============================================================================
//  x402 service catalog bridge for the AI chat path.
//
//  The web settings UI carries a hard-coded BUILTIN list of paid x402
//  services (see server.cpp). Users flip toggles to enable any subset, and
//  can register custom ones too. This module mirrors that catalog on the
//  C++ side so the chat loop can:
//
//    1. Turn enabled services into OpenAI `tools[]` descriptors the model
//       sees, and
//    2. Dispatch any `tool_calls` the model emits by firing the appropriate
//       x402 GET/POST and returning the raw response to the model.
// ============================================================================
#pragma once
#include <Arduino.h>

// Build the OpenAI-compatible tools[] JSON array for every currently enabled
// service endpoint. Returns "" if nothing is enabled — in which case the
// caller should omit the tools field entirely (sending an empty array makes
// some providers error out).
String servicesBuildToolsArray();

// Execute one tool call the model emitted. `name` is the function the model
// picked (format: "<serviceId>__<endpointId>"), `argsJson` is the arguments
// object as a JSON string. Returns the raw response body from the remote
// service on success, or a small JSON error envelope on failure. The return
// value is always a string the model can reason about.
//
// `outCost`, if non-null, receives the USDC amount we paid for this call.
String servicesDispatchToolCall(const String &name,
                                const String &argsJson,
                                double *outCost = nullptr);

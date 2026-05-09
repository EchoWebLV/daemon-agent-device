// ---------------------------------------------------------------------------
//  Skill tools — generic OpenAI-tool handlers exposed to the LLM whenever
//  at least one markdown-kind skill is enabled.
//
//  These are skill-agnostic primitives. The skill's markdown body teaches
//  the model how to chain them (e.g. SP3ND: registerAgent -> cart -> order
//  -> payAgentOrder -> x402_pay -> getPartnerOrders).
//
//  Each handler:
//    - Takes the OpenAI-format `arguments` JSON string for the call
//    - Writes a JSON response into `out` (NUL-terminated, truncated on
//      overflow)
//    - Returns 0 on success or non-zero on transport failure (the JSON
//      itself describes skill-level success/failure via {"ok":..,"error":..})
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int skill_tool_http_request     (const char *args_json, char *out, size_t cap);
int skill_tool_x402_pay         (const char *args_json, char *out, size_t cap);
int skill_tool_secret_get       (const char *args_json, char *out, size_t cap);
int skill_tool_secret_set       (const char *args_json, char *out, size_t cap);
int skill_tool_solana_get_pubkey(const char *args_json, char *out, size_t cap);

// True if `name` is one of the five generic skill tools above. Used by
// ai.c's execute_tool to dispatch.
bool skill_tools_is_skill_tool(const char *name);

// Dispatch by name. `name` must satisfy skill_tools_is_skill_tool().
int skill_tools_dispatch(const char *name, const char *args_json,
                         char *out, size_t cap);

#ifdef __cplusplus
}
#endif

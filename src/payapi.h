// ---------------------------------------------------------------------------
// payapi.h — pay.sh dispatcher
//
// Boots a background task on wifi-up that fetches the pay.sh catalog and the
// openapi spec for each enabled provider, slims them into LLM tool defs, and
// hands them to ai.c via attach_tools(). Also implements the spending guard
// callback that ai.c passes through x402_call_with_guard().
// ---------------------------------------------------------------------------
#ifndef PAYAPI_H
#define PAYAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x402.h"   // for x402_guard_decision_t

#ifdef __cplusplus
extern "C" {
#endif

// Lifecycle. Call once after wifi_sta_got_ip event; idempotent — re-calls
// retrigger a refresh of all enabled providers' openapi specs.
void payapi_init(void);

// Force-refresh the catalog only. Web admin "Refresh catalog" button.
bool payapi_refresh_catalog(void);

// Force-refresh one provider's openapi. Web admin per-provider Retry link.
bool payapi_refresh_provider(const char *fqn);

// Tool resolution. Called from ai.c execute_tool() when an LLM tool name
// matches a registered pay.sh tool. Fields are caller-owned copies; no
// registry pointer aliases survive the call.
typedef struct {
    char     service_url[256];
    char     method[8];
    char     path[256];
    char     fqn[64];
    uint32_t price_usd_max_cents;
} payapi_tool_info_t;

bool payapi_resolve(const char *tool_name, payapi_tool_info_t *out);

// Tool registration. Called from ai.c attach_tools(); appends pay.sh-derived
// tool definitions to the cJSON array. Returns the number of tools appended.
struct cJSON;
int payapi_attach_tools(struct cJSON *out_array);

// Guard callback. Pass to x402_call_with_guard(). Reads spending caps
// from devcfg, applies the auto/confirm/refuse policy, opens the
// pay_confirm_screen modal when needed.
x402_guard_decision_t payapi_guard(uint64_t actual_micros,
                                   const char *description,
                                   void *user);

// Status JSON for /api/services. Returns a malloc'd cJSON tree the
// caller must cJSON_Delete.
struct cJSON *payapi_status_json(void);

#ifdef __cplusplus
}
#endif
#endif  // PAYAPI_H

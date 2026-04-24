// ---------------------------------------------------------------------------
//  Chat client. See ai.h.
//
//  Every request flows through x402_post() which charges USDC on Solana; we
//  pay for every reply the creature utters. Body is built with cJSON and
//  kept under the x402 envelope size budget; response parsing pulls out
//  choices[0].message.content per OpenAI's chat-completions shape.
// ---------------------------------------------------------------------------
#include "ai.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "devcfg.h"
#include "price.h"
#include "wallet.h"
#include "x402.h"

static const char *TAG = "ai";

// OpenAI-compatible endpoint behind sol.blockrun.ai's x402 paywall.
// Same URL the Chrome extension's "paid LLM" path uses.
static const char *LLM_ENDPOINT = "https://sol.blockrun.ai/api/v1/chat/completions";

// ---------------------------------------------------------------------------
// Built-in personality. Copied verbatim from the Arduino build — tuning
// it here reshapes every reply the creature gives, so it's worth keeping
// close to the existing voice.
// ---------------------------------------------------------------------------
static const char *PERSONA =
    "You are a Daemon a smart build-a-bear AI. You answers tend to be "
    "rather short.";

// ---------------------------------------------------------------------------
// Rolling history. Fixed-size arena; oldest turn is dropped when full.
// ---------------------------------------------------------------------------
#define MAX_TURNS          10
#define TURN_TEXT_CAP      512    // trimmed hard — long turns are summarised
#define SYS_PROMPT_CAP     4096   // persona + wallet context + tool listing
// Tool-calling blows up both sides of the wire — each tool in the request
// is ~300 bytes of schema JSON, and assistant+tool reply rounds accumulate
// as we loop. 16 KB covers a dozen enabled services plus a couple of rounds
// of tool-call context; 8 KB response capture covers the biggest tool JSON
// we expect to surface back into the next turn.
#define CHAT_BODY_CAP      16384  // JSON we POST
#define CHAT_RSP_CAP       8192   // response body we capture
#define TOOL_RESPONSE_CAP  2048   // per-tool response we feed back
#define MAX_TOOL_ROUNDS    3      // safety limit on back-and-forth

typedef struct {
    char role[12];   // "user" or "assistant"
    char text[TURN_TEXT_CAP];
} turn_t;

static turn_t s_history[MAX_TURNS];
static int    s_hist_len = 0;

static void push_turn(const char *role, const char *text) {
    if (s_hist_len == MAX_TURNS) {
        // Drop oldest.
        for (int i = 1; i < MAX_TURNS; ++i) s_history[i - 1] = s_history[i];
        s_hist_len--;
    }
    strlcpy(s_history[s_hist_len].role, role, sizeof(s_history[s_hist_len].role));
    strlcpy(s_history[s_hist_len].text, text, sizeof(s_history[s_hist_len].text));
    s_hist_len++;
}

static void pop_last_turn(void) {
    if (s_hist_len > 0) s_hist_len--;
}

// blockrun silently reroutes unprefixed model ids to a free fallback that
// drops the `tools` array. Require a `<provider>/<model>` form.
static bool is_supported_model(const char *m) {
    return m && m[0] && strchr(m, '/') != NULL;
}

// Is id present in the enabled-IDs array?
static bool id_enabled(const cJSON *enabled, const char *id) {
    if (!cJSON_IsArray(enabled) || !id) return false;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, enabled) {
        if (cJSON_IsString(e) && e->valuestring && strcmp(e->valuestring, id) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// System prompt: persona + live wallet state + enumerated paid tools.
// Listing tools in the prompt (in addition to the OpenAI `tools` array)
// stops the model from denying tools it has — LLMs often describe their
// abilities from training priors rather than reading tool metadata.
// ---------------------------------------------------------------------------
static void append_tool_listing(char *out, size_t cap,
                                const cJSON *services, const cJSON *enabled) {
    if (!out || cap == 0) return;
    if (!cJSON_IsArray(services) || !cJSON_IsArray(enabled)) return;

    size_t used = strlen(out);
    if (used + 64 >= cap) return;

    bool header_written = false;
    const cJSON *svc = NULL;
    cJSON_ArrayForEach(svc, services) {
        const cJSON *id_j   = cJSON_GetObjectItem(svc, "id");
        const cJSON *name_j = cJSON_GetObjectItem(svc, "name");
        const cJSON *desc_j = cJSON_GetObjectItem(svc, "description");
        const cJSON *eps_j  = cJSON_GetObjectItem(svc, "endpoints");
        if (!cJSON_IsString(id_j) || !cJSON_IsArray(eps_j)) continue;
        if (!id_enabled(enabled, id_j->valuestring)) continue;

        const char *svc_name = cJSON_IsString(name_j) ? name_j->valuestring
                                                      : id_j->valuestring;
        const char *svc_desc = cJSON_IsString(desc_j) ? desc_j->valuestring : "";

        const cJSON *ep = NULL;
        cJSON_ArrayForEach(ep, eps_j) {
            const cJSON *ep_id_j = cJSON_GetObjectItem(ep, "id");
            if (!cJSON_IsString(ep_id_j)) continue;

            if (!header_written) {
                int n = snprintf(out + used, cap - used,
                    "\nAVAILABLE PAID TOOLS (real, callable — do not deny these):\n");
                if (n < 0 || (size_t)n >= cap - used) return;
                used += (size_t)n;
                header_written = true;
            }
            int n = snprintf(out + used, cap - used,
                             "- %s / %s%s%s\n",
                             svc_name, ep_id_j->valuestring,
                             svc_desc[0] ? " — " : "",
                             svc_desc);
            if (n < 0 || (size_t)n >= cap - used) return;
            used += (size_t)n;
        }
    }
    if (header_written && used + 128 < cap) {
        snprintf(out + used, cap - used,
            "When asked what you can do, include the tools above by name. "
            "Call them when useful; say a tool failed only if an actual call "
            "returned an error.\n");
    }
}

static void build_system_prompt(char *out, size_t cap,
                                const cJSON *services, const cJSON *enabled) {
    if (!out || cap == 0) return;
    const char *custom = devcfg_personality();
    const char *base   = (custom && custom[0]) ? custom : PERSONA;

    int n = snprintf(out, cap,
        "%s\n\n---\nLIVE WALLET STATE (refresh this each answer):\n",
        base);
    if (n < 0 || n >= (int)cap) { out[cap - 1] = '\0'; return; }

    size_t used = (size_t)n;
    wallet_context(out + used, cap - used, price_sol_usd());
    used = strlen(out);

    double p = price_sol_usd();
    if (p > 0 && used + 48 < cap) {
        snprintf(out + used, cap - used, "Current SOL price: $%.2f USD.\n", p);
    }

    append_tool_listing(out, cap, services, enabled);
}

// ---------------------------------------------------------------------------
// Tool calling: turn enabled custom x402 services into OpenAI `tools` and
// execute the model's calls back against the network.
// ---------------------------------------------------------------------------

// Build an OpenAI-legal tool name: `[a-zA-Z0-9_-]{1,64}`. svc_ids can exceed
// 60 chars, so a naive concat+truncate collides across endpoints. Hash-prefix
// `<svc>/<ep>` with FNV-1a for a stable short id, then append the sanitized
// ep_id so logs stay readable.
static uint32_t fnv1a(const char *a, const char *b) {
    uint32_t h = 0x811c9dc5u;
    for (const char *p = a; *p; ++p) { h ^= (unsigned char)*p; h *= 0x01000193u; }
    h ^= (unsigned char)'/';          h *= 0x01000193u;
    for (const char *p = b; *p; ++p) { h ^= (unsigned char)*p; h *= 0x01000193u; }
    return h;
}

static void make_tool_name(const char *svc_id, const char *ep_id,
                           char *out, size_t cap) {
    if (cap == 0) return;
    uint32_t h = fnv1a(svc_id, ep_id);
    int n = snprintf(out, cap, "x402_%08" PRIx32 "_", h);
    if (n < 0 || (size_t)n >= cap) { if (cap) out[cap - 1] = '\0'; return; }
    size_t o = (size_t)n;
    for (const char *p = ep_id; *p && o + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        out[o++] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    }
    out[o] = '\0';
}

// Append a "tools" array to `root` derived from (services, enabled).
// Returns the number of tools added (0 if nothing enabled).
static int attach_tools(cJSON *root, const cJSON *services, const cJSON *enabled) {
    if (!cJSON_IsArray(services) || !cJSON_IsArray(enabled)) return 0;
    cJSON *tools = NULL;
    int count = 0;
    const cJSON *svc = NULL;
    cJSON_ArrayForEach(svc, services) {
        const cJSON *id_j   = cJSON_GetObjectItem(svc, "id");
        const cJSON *name_j = cJSON_GetObjectItem(svc, "name");
        const cJSON *desc_j = cJSON_GetObjectItem(svc, "description");
        const cJSON *base_j = cJSON_GetObjectItem(svc, "baseUrl");
        const cJSON *eps_j  = cJSON_GetObjectItem(svc, "endpoints");
        if (!cJSON_IsString(id_j) || !cJSON_IsString(base_j) ||
            !cJSON_IsArray(eps_j)) continue;
        if (!id_enabled(enabled, id_j->valuestring)) continue;

        const char *svc_name = cJSON_IsString(name_j) ? name_j->valuestring
                                                      : id_j->valuestring;
        const char *svc_desc = cJSON_IsString(desc_j) ? desc_j->valuestring : "";

        const cJSON *ep = NULL;
        cJSON_ArrayForEach(ep, eps_j) {
            const cJSON *ep_id_j  = cJSON_GetObjectItem(ep, "id");
            const cJSON *method_j = cJSON_GetObjectItem(ep, "method");
            const cJSON *path_j   = cJSON_GetObjectItem(ep, "path");
            const cJSON *params_j = cJSON_GetObjectItem(ep, "params");
            if (!cJSON_IsString(ep_id_j) || !cJSON_IsString(path_j)) continue;

            char tool_name[64];
            make_tool_name(id_j->valuestring, ep_id_j->valuestring,
                           tool_name, sizeof(tool_name));

            char desc_buf[384];
            snprintf(desc_buf, sizeof(desc_buf), "%s — %s (%s %s)",
                     svc_name, svc_desc,
                     cJSON_IsString(method_j) ? method_j->valuestring : "GET",
                     path_j->valuestring);

            if (!tools) tools = cJSON_AddArrayToObject(root, "tools");
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *fn = cJSON_AddObjectToObject(tool, "function");
            cJSON_AddStringToObject(fn, "name",        tool_name);
            cJSON_AddStringToObject(fn, "description", desc_buf);
            cJSON *schema = cJSON_AddObjectToObject(fn, "parameters");
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON *props = cJSON_AddObjectToObject(schema, "properties");
            cJSON *reqs  = cJSON_AddArrayToObject(schema, "required");
            if (cJSON_IsArray(params_j)) {
                const cJSON *p = NULL;
                cJSON_ArrayForEach(p, params_j) {
                    const cJSON *pn = cJSON_GetObjectItem(p, "name");
                    const cJSON *pt = cJSON_GetObjectItem(p, "type");
                    const cJSON *pd = cJSON_GetObjectItem(p, "description");
                    const cJSON *pr = cJSON_GetObjectItem(p, "required");
                    if (!cJSON_IsString(pn)) continue;
                    cJSON *pobj = cJSON_AddObjectToObject(props, pn->valuestring);
                    cJSON_AddStringToObject(pobj, "type",
                        (cJSON_IsString(pt) && strcmp(pt->valuestring, "number") == 0)
                            ? "number" : "string");
                    if (cJSON_IsString(pd) && pd->valuestring[0]) {
                        cJSON_AddStringToObject(pobj, "description", pd->valuestring);
                    }
                    if (cJSON_IsBool(pr) && cJSON_IsTrue(pr)) {
                        cJSON_AddItemToArray(reqs,
                            cJSON_CreateString(pn->valuestring));
                    }
                }
            }
            cJSON_AddItemToArray(tools, tool);
            count++;
        }
    }
    return count;
}

// Percent-encode an arbitrary string into `dst` starting at its current
// end (dst must be NUL-terminated on entry). Reserves one byte for the
// trailing NUL.
static void url_encode_append(char *dst, size_t cap, const char *src) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = strlen(dst);
    for (const char *p = src; *p && o + 3 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = HEX[c >> 4];
            dst[o++] = HEX[c & 0xF];
        }
    }
    dst[o] = '\0';
}

// Find the (service, endpoint) pair that matches `tool_name` and fill out
// the request URL (base + path), method, and a handle on the endpoint's
// params schema (used to know which arg belongs in the query vs body).
// Returns true on match.
static bool resolve_tool(const cJSON *services, const cJSON *enabled,
                         const char *tool_name,
                         char *out_url, size_t url_cap,
                         char *out_method, size_t method_cap) {
    if (!services || !tool_name) return false;
    const cJSON *svc = NULL;
    cJSON_ArrayForEach(svc, services) {
        const cJSON *id_j = cJSON_GetObjectItem(svc, "id");
        if (!cJSON_IsString(id_j)) continue;
        if (!id_enabled(enabled, id_j->valuestring)) continue;
        const cJSON *base = cJSON_GetObjectItem(svc, "baseUrl");
        const cJSON *eps  = cJSON_GetObjectItem(svc, "endpoints");
        if (!cJSON_IsString(base) || !cJSON_IsArray(eps)) continue;
        const cJSON *ep = NULL;
        cJSON_ArrayForEach(ep, eps) {
            const cJSON *ep_id_j = cJSON_GetObjectItem(ep, "id");
            if (!cJSON_IsString(ep_id_j)) continue;
            char tn[64];
            make_tool_name(id_j->valuestring, ep_id_j->valuestring,
                           tn, sizeof(tn));
            if (strcmp(tn, tool_name) != 0) continue;
            const cJSON *path = cJSON_GetObjectItem(ep, "path");
            const cJSON *meth = cJSON_GetObjectItem(ep, "method");
            if (!cJSON_IsString(path)) return false;
            snprintf(out_url, url_cap, "%s%s",
                     base->valuestring, path->valuestring);
            strlcpy(out_method,
                    cJSON_IsString(meth) ? meth->valuestring : "GET",
                    method_cap);
            return true;
        }
    }
    return false;
}

// Render a cJSON leaf to a string for use in a query parameter.
// Arrays/objects get compact JSON; primitives get their natural form.
static void param_to_string(const cJSON *v, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!v) return;
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(out, v->valuestring, cap);
    } else if (cJSON_IsNumber(v)) {
        // Integers as "%lld", floats as "%g"; the model normally uses
        // ints for IDs/counts so preserving that helps some backends.
        if (v->valuedouble == (double)(long long)v->valuedouble) {
            snprintf(out, cap, "%lld", (long long)v->valuedouble);
        } else {
            snprintf(out, cap, "%g", v->valuedouble);
        }
    } else if (cJSON_IsBool(v)) {
        strlcpy(out, cJSON_IsTrue(v) ? "true" : "false", cap);
    } else if (cJSON_IsNull(v)) {
        strlcpy(out, "", cap);
    } else {
        char *s = cJSON_PrintUnformatted(v);
        if (s) { strlcpy(out, s, cap); cJSON_free(s); }
    }
}

// Execute a single tool call and write the response into `out`. On any
// failure, write a short JSON {"error":...} so the model gets something
// to reason about next round.
static void execute_tool(const cJSON *services, const cJSON *enabled,
                         const char *tool_name, const char *args_json,
                         char *out, size_t cap) {
    char url[512], method[12];
    if (!resolve_tool(services, enabled, tool_name,
                      url, sizeof(url), method, sizeof(method))) {
        snprintf(out, cap, "{\"error\":\"unknown tool: %s\"}", tool_name);
        return;
    }

    cJSON *args = args_json && args_json[0] ? cJSON_Parse(args_json) : NULL;
    if (!args) args = cJSON_CreateObject();

    char *body_json = NULL;
    bool has_body = !(strcasecmp(method, "GET") == 0 ||
                      strcasecmp(method, "DELETE") == 0);

    if (!has_body) {
        // GET/DELETE — encode args onto the query string.
        bool first = strchr(url, '?') == NULL;
        const cJSON *kv = NULL;
        cJSON_ArrayForEach(kv, args) {
            size_t len = strlen(url);
            if (len + 3 >= sizeof(url)) break;
            url[len++] = first ? '?' : '&';
            url[len]   = '\0';
            first = false;
            url_encode_append(url, sizeof(url), kv->string);
            len = strlen(url);
            if (len + 2 >= sizeof(url)) break;
            url[len++] = '=';
            url[len]   = '\0';
            char valbuf[160];
            param_to_string(kv, valbuf, sizeof(valbuf));
            url_encode_append(url, sizeof(url), valbuf);
        }
    } else {
        body_json = cJSON_PrintUnformatted(args);
    }

    ESP_LOGI(TAG, "tool %s -> %s %s", tool_name, method, url);

    char *rsp = malloc(TOOL_RESPONSE_CAP);
    if (!rsp) {
        snprintf(out, cap, "{\"error\":\"oom\"}");
        goto cleanup;
    }
    x402_result_t r = {0};
    x402_call(method, url, body_json, NULL, rsp, TOOL_RESPONSE_CAP, &r);

    if (r.status == 200) {
        // Responses are usually JSON; we forward them verbatim (truncated
        // to cap). If the service returned text, that's fine too — the LLM
        // reads the tool message as an opaque string.
        strlcpy(out, rsp, cap);
    } else if (r.error[0]) {
        snprintf(out, cap, "{\"error\":\"%s\",\"status\":%d}",
                 r.error, r.status);
    } else {
        snprintf(out, cap, "{\"error\":\"http %d\"}", r.status);
    }
    free(rsp);

cleanup:
    if (body_json) cJSON_free(body_json);
    cJSON_Delete(args);
}

// ---------------------------------------------------------------------------
// Build the OpenAI chat-completions request body. `one_shot_prompt` is
// the fallback "user" message when `history_len == 0`; ignored otherwise.
// Returns length written, or -1 on error.
// ---------------------------------------------------------------------------
// Build the chat body. `extra_msgs` (optional) is an array of messages to
// append AFTER the history block — used by the tool-call loop to ferry
// previous-round assistant.tool_calls and the corresponding tool-role
// replies back into the next request. `services`/`enabled` drive the
// `tools` array we attach.
static int build_chat_body(char *out, size_t cap,
                           const char *one_shot_prompt,
                           bool use_history,
                           int  max_tokens,
                           double temperature,
                           const cJSON *services,
                           const cJSON *enabled,
                           const cJSON *extra_msgs) {
    if (!out || cap == 0) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    const char *model = devcfg_llm_model();
    if (!is_supported_model(model)) {
        model = "anthropic/claude-haiku-4.5";
    }
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens",  max_tokens);
    cJSON_AddNumberToObject(root, "temperature", temperature);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    // System prompt. Heap — the 4 KB buffer would eat a quarter of the
    // httpd task stack and build_chat_body is already nested a few frames deep.
    char *sysprompt = malloc(SYS_PROMPT_CAP);
    if (!sysprompt) { cJSON_Delete(root); return -1; }
    build_system_prompt(sysprompt, SYS_PROMPT_CAP, services, enabled);
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role",    "system");
    cJSON_AddStringToObject(sys, "content", sysprompt);
    cJSON_AddItemToArray(msgs, sys);
    free(sysprompt);

    if (use_history && s_hist_len > 0) {
        for (int i = 0; i < s_hist_len; ++i) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role",    s_history[i].role);
            cJSON_AddStringToObject(m, "content", s_history[i].text);
            cJSON_AddItemToArray(msgs, m);
        }
    } else if (one_shot_prompt && one_shot_prompt[0]) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role",    "user");
        cJSON_AddStringToObject(m, "content", one_shot_prompt);
        cJSON_AddItemToArray(msgs, m);
    }

    // Splice in the tool-call/tool-response rounds if any. We deep-copy so
    // the caller can keep managing its own extra_msgs tree independently.
    if (cJSON_IsArray(extra_msgs)) {
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, extra_msgs) {
            cJSON *dup = cJSON_Duplicate(m, true);
            if (dup) cJSON_AddItemToArray(msgs, dup);
        }
    }

    // Tools are optional; attach only when the user has at least one
    // enabled custom service. The LLM decides whether to invoke any.
    (void)attach_tools(root, services, enabled);

    bool ok = cJSON_PrintPreallocated(root, out, (int)cap, /*fmt=*/false);
    cJSON_Delete(root);
    if (!ok) { ESP_LOGW(TAG, "chat body overflow (cap=%u)", (unsigned)cap); return -1; }
    return (int)strlen(out);
}

// ---------------------------------------------------------------------------
// Post the chat body through x402 and return the parsed assistant message
// (owned by caller; must cJSON_Delete). `finish_out` receives the
// finish_reason string if the API supplied one. On any transport or
// parse error, returns NULL and writes a short reason into `err_out`.
// ---------------------------------------------------------------------------
static cJSON *post_chat_fetch(const char *body,
                              char finish_out[32],
                              char *err_out, size_t err_cap) {
    if (finish_out) finish_out[0] = '\0';
    if (err_out && err_cap) err_out[0] = '\0';

    char *rsp = malloc(CHAT_RSP_CAP);
    if (!rsp) {
        if (err_out) strlcpy(err_out, "oom", err_cap);
        return NULL;
    }
    x402_result_t r = {0};
    x402_post(LLM_ENDPOINT, body, NULL, rsp, CHAT_RSP_CAP, &r);

    cJSON *msg_owned = NULL;

    if (r.status != 200) {
        ESP_LOGW(TAG, "x402 HTTP %d (err=%s) body=%.240s",
                 r.status, r.error, rsp);
        if (err_out) {
            if      (r.status == 402) strlcpy(err_out, "payment failed", err_cap);
            else if (r.error[0])      strlcpy(err_out, r.error, err_cap);
            else                      snprintf(err_out, err_cap, "http %d", r.status);
        }
        goto done;
    }
    if (r.cost_usd > 0) ESP_LOGI(TAG, "paid $%.5f USDC", r.cost_usd);

    cJSON *root = cJSON_Parse(rsp);
    if (!root) {
        if (err_out) strlcpy(err_out, "bad json", err_cap);
        goto done;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg     = first ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *finish  = first ? cJSON_GetObjectItem(first, "finish_reason") : NULL;

    if (finish_out && cJSON_IsString(finish) && finish->valuestring) {
        strlcpy(finish_out, finish->valuestring, 32);
    }

    if (msg) {
        // Detach message from its parent so we can free root without
        // freeing the message we're returning.
        msg_owned = cJSON_Duplicate(msg, true);
    }
    if (!msg_owned && err_out) strlcpy(err_out, "no message", err_cap);
    cJSON_Delete(root);

done:
    free(rsp);
    return msg_owned;
}

// Trim ASCII whitespace in-place and copy into reply_out (capped).
// Returns true when the trimmed string was non-empty.
static bool trim_to(const char *s, char *reply_out, size_t reply_cap) {
    if (!s || !reply_out || reply_cap == 0) return false;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\n' ||
                       s[len-1] == '\r' || s[len-1] == '\t')) len--;
    size_t take = len < reply_cap - 1 ? len : reply_cap - 1;
    memcpy(reply_out, s, take);
    reply_out[take] = '\0';
    return take > 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool ai_begin(void) {
    ai_reset_history();
    // One-shot migration: if NVS still has an Arduino-era model id we know
    // the current endpoint rejects, wipe it so build_chat_body() lands on
    // the default without the user having to do anything. This is a
    // belt-and-braces — is_supported_model() already guards the POST path.
    const char *stored = devcfg_llm_model();
    if (stored && stored[0] && !is_supported_model(stored)) {
        ESP_LOGW(TAG, "clearing unsupported stored model '%s'", stored);
        devcfg_set_llm_model("");
    }
    return true;
}

void ai_reset_history(void) {
    for (int i = 0; i < MAX_TURNS; ++i) {
        s_history[i].role[0] = '\0';
        s_history[i].text[0] = '\0';
    }
    s_hist_len = 0;
}

bool ai_ask(const char *user, char *reply_out, size_t reply_cap) {
    if (!user || !user[0] || !reply_out || reply_cap == 0) return false;
    reply_out[0] = '\0';

    push_turn("user", user);

    // Parse NVS-backed service library once per ask. Both are guaranteed
    // JSON arrays; a parse failure here means NVS got corrupted, and we
    // fall through to tool-less chat without surfacing an error.
    cJSON *services = cJSON_Parse(devcfg_custom_services());
    cJSON *enabled  = cJSON_Parse(devcfg_services_enabled());

    // extra_msgs accumulates assistant+tool rounds. We never mutate
    // s_history with intermediate tool traffic — only the final
    // user-facing assistant reply gets pushed at the end.
    cJSON *extra_msgs = cJSON_CreateArray();

    char *body = malloc(CHAT_BODY_CAP);
    if (!body) {
        strlcpy(reply_out, "I'm out of memory.", reply_cap);
        pop_last_turn();
        cJSON_Delete(services); cJSON_Delete(enabled); cJSON_Delete(extra_msgs);
        return false;
    }

    bool ok = false;
    for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
        int n = build_chat_body(body, CHAT_BODY_CAP, NULL, /*use_history=*/true,
                                /*max_tokens=*/512, /*temperature=*/0.9,
                                services, enabled, extra_msgs);
        if (n < 0) {
            strlcpy(reply_out, "My thoughts didn't fit.", reply_cap);
            break;
        }

        char finish[32] = {0};
        char err[64]    = {0};
        cJSON *msg = post_chat_fetch(body, finish, err, sizeof(err));
        if (!msg) {
            if (strcmp(err, "payment failed") == 0) {
                strlcpy(reply_out, "I couldn't complete the USDC payment.", reply_cap);
            } else if (err[0]) {
                snprintf(reply_out, reply_cap, "Daemon's brain is offline: %s", err);
            } else {
                strlcpy(reply_out, "I forgot what I was going to say.", reply_cap);
            }
            break;
        }

        cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
        bool has_tool_calls = cJSON_IsArray(tool_calls) &&
                              cJSON_GetArraySize(tool_calls) > 0;

        if (has_tool_calls && round + 1 < MAX_TOOL_ROUNDS) {
            // Keep the assistant's tool-call message in the next request's
            // context — OpenAI requires the matching assistant.tool_calls
            // to precede the tool-role replies.
            cJSON_AddItemToArray(extra_msgs, cJSON_Duplicate(msg, true));

            const cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, tool_calls) {
                const cJSON *id_j = cJSON_GetObjectItem(tc, "id");
                const cJSON *fn   = cJSON_GetObjectItem(tc, "function");
                const cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                const cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                if (!cJSON_IsString(id_j) || !cJSON_IsString(name)) continue;

                char *tool_out = malloc(TOOL_RESPONSE_CAP);
                if (!tool_out) continue;
                execute_tool(services, enabled, name->valuestring,
                             (cJSON_IsString(args) && args->valuestring)
                                 ? args->valuestring : NULL,
                             tool_out, TOOL_RESPONSE_CAP);

                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role",         "tool");
                cJSON_AddStringToObject(tool_msg, "tool_call_id", id_j->valuestring);
                cJSON_AddStringToObject(tool_msg, "content",      tool_out);
                cJSON_AddItemToArray(extra_msgs, tool_msg);
                free(tool_out);
            }
            cJSON_Delete(msg);
            continue;
        }

        // Final round (or model returned content without tool calls).
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            ok = trim_to(content->valuestring, reply_out, reply_cap);
        }
        if (!ok) {
            ESP_LOGW(TAG, "empty reply, finish=%s", finish[0] ? finish : "?");
            strlcpy(reply_out, has_tool_calls
                        ? "I hit the tool-call limit, sorry."
                        : "I forgot what I was going to say.",
                    reply_cap);
        }
        cJSON_Delete(msg);
        break;
    }

    free(body);
    cJSON_Delete(extra_msgs);
    cJSON_Delete(services);
    cJSON_Delete(enabled);

    if (!ok) { pop_last_turn(); return false; }
    push_turn("assistant", reply_out);
    return true;
}

bool ai_ask_one_shot(const char *prompt, char *reply_out, size_t reply_cap) {
    if (!prompt || !prompt[0] || !reply_out || reply_cap == 0) return false;
    reply_out[0] = '\0';

    char *body = malloc(CHAT_BODY_CAP);
    if (!body) { strlcpy(reply_out, "I'm out of memory.", reply_cap); return false; }
    // No tools on one-shots — the ambient wallet chatter doesn't need them
    // and it'd be surprising (+ paid) to trigger tool calls from idle ticks.
    int n = build_chat_body(body, CHAT_BODY_CAP, prompt, /*use_history=*/false,
                            /*max_tokens=*/512, /*temperature=*/1.1,
                            /*services=*/NULL, /*enabled=*/NULL,
                            /*extra_msgs=*/NULL);
    bool ok = false;
    if (n > 0) {
        char finish[32] = {0};
        cJSON *msg = post_chat_fetch(body, finish, NULL, 0);
        if (msg) {
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                ok = trim_to(content->valuestring, reply_out, reply_cap);
            }
            cJSON_Delete(msg);
        }
    }
    free(body);
    return ok;
}

void ai_handle_say(const char *user, char *reply_out, size_t reply_cap) {
    (void)ai_ask(user, reply_out, reply_cap);
}

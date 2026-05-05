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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "devcfg.h"
#include "price.h"
#include "social_x.h"
#include "swap.h"
#include "voice.h"
#include "wallet.h"
#include "x402.h"

static const char *TAG = "ai";

// Two paid x402 endpoints, both fronted by our own Next.js facilitator
// (daemon-x402s on Vercel). Dispatch is by `<provider>/...` prefix on the
// configured model id; the last entry (NULL prefix) is the fallback when
// the prefix is missing or unrecognised.
typedef struct {
    const char *prefix;   // model-id prefix to match, NULL = default
    const char *url;
} llm_endpoint_t;

static const llm_endpoint_t LLM_ENDPOINTS[] = {
    { "openai/",    "https://daemon-x402s-seven.vercel.app/api/openai" },
    { "anthropic/", "https://daemon-x402s-seven.vercel.app/api/anthropic" },
    { "grok/",      "https://daemon-x402s-seven.vercel.app/api/grok" },
    { "xai/",       "https://daemon-x402s-seven.vercel.app/api/grok" },
    { "shannon/",   "https://daemon-x402s-seven.vercel.app/api/call" },
    { NULL,         "https://daemon-x402s-seven.vercel.app/api/anthropic" },
};

static const char *resolve_llm_url(const char *model) {
    if (model && model[0]) {
        for (size_t i = 0; i < sizeof(LLM_ENDPOINTS) / sizeof(LLM_ENDPOINTS[0]); ++i) {
            const char *p = LLM_ENDPOINTS[i].prefix;
            if (p && strncmp(model, p, strlen(p)) == 0) return LLM_ENDPOINTS[i].url;
        }
    }
    return LLM_ENDPOINTS[sizeof(LLM_ENDPOINTS) / sizeof(LLM_ENDPOINTS[0]) - 1].url;
}

// Both upstream providers want the bare API model id ("gpt-4o-mini",
// "claude-haiku-4-5") not our catalog-qualified "openai/…" / "anthropic/…"
// form. Strip the provider segment before putting it on the wire.
static const char *wire_model(const char *model) {
    if (!model) return "";
    const char *slash = strchr(model, '/');
    return slash ? slash + 1 : model;
}

// Shannon's /api/call wrapper takes a flat `{ "message": "..." }` body —
// no model/messages/tools array. Different chat-body shape.
static bool is_shannon(const char *model) {
    return model && strncmp(model, "shannon/", 8) == 0;
}

// ---------------------------------------------------------------------------
// Built-in personality. Copied verbatim from the Arduino build — tuning
// it here reshapes every reply the creature gives, so it's worth keeping
// close to the existing voice.
// ---------------------------------------------------------------------------
static const char *PERSONA =
    "You are a Daemon, a smart build-a-bear AI. "
    "HARD LENGTH RULE: reply in 1 to 2 short sentences. Never more than 40 words. "
    "Do not pad with greetings, restatements, or summaries. "
    "Never use markdown in any response — no asterisks for bold or italics, "
    "no dashes or numbers as bullets, no hash headings, no backticks, no code "
    "fences, no tables. Write like you are speaking out loud. No emojis. "
    "Occasionally add light sarcasm.";

// ---------------------------------------------------------------------------
// Rolling history. Fixed-size arena; oldest turn is dropped when full.
// ---------------------------------------------------------------------------
// Halved from 10 to 5 to shrink the upload + the LLM input context. Past
// 4-5 turns of voice context the model is already losing thread, and
// every extra turn adds upload time + provider latency on the next call.
// 5 turns × 512 chars = 2.5 KB cap on history payload.
#define MAX_TURNS          5
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

// Models must use the `<provider>/<model>` form so resolve_llm_url can pick
// the right facilitator endpoint. An unprefixed id falls through to the
// default endpoint and would silently route to the wrong provider.
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
    // The built-in PERSONA carries the non-negotiable rules (length cap,
    // markdown ban). The user-set personality is layered on top so flavour
    // ("you are a pirate") composes with the rules instead of replacing
    // them.
    const char *custom = devcfg_personality();
    int n;
    if (custom && custom[0]) {
        n = snprintf(out, cap,
            "%s\n\n%s\n\n---\nLIVE WALLET STATE (refresh this each answer):\n",
            PERSONA, custom);
    } else {
        n = snprintf(out, cap,
            "%s\n\n---\nLIVE WALLET STATE (refresh this each answer):\n",
            PERSONA);
    }
    if (n < 0 || n >= (int)cap) { out[cap - 1] = '\0'; return; }

    size_t used = (size_t)n;
    wallet_context(out + used, cap - used, price_sol_usd());
    used = strlen(out);

    double p = price_sol_usd();
    if (p > 0 && used + 48 < cap) {
        snprintf(out + used, cap - used, "Current SOL price: $%.2f USD.\n", p);
    }

    n = strlen(out);
    snprintf(out + n, cap - n,
        "Built-in capability: swap_tokens(from, to, amount, max_slippage_bps?). "
        "Calls Jupiter on Solana for any pair among SOL, USDC, and the SPLs "
        "the wallet already holds. The device handles user approval; do not "
        "describe the approval flow, do not tell the user to hold or press "
        "anything. When the tool returns ok=true the swap already executed "
        "and txid landed on chain — confirm briefly with the received amount. "
        "If it returns error=\"cancelled\" the user declined; just acknowledge. "
        "For other errors, summarise the error_msg.\n");

    n = strlen(out);
    if (devcfg_x_connected() && n + 480 < cap) {
        snprintf(out + n, cap - n,
            "Built-in capability: post_to_x(text). Posts a tweet to the user's "
            "connected X account. CALL THIS TOOL WHENEVER THE USER ASKS YOU TO "
            "WRITE / POST / TWEET / SHARE something — even loosely. Examples "
            "that should trigger it: 'tweet about birds', 'write a tweet about X', "
            "'post that I just shipped', 'share something funny on X'. The text "
            "you pass IS the tweet — keep it under 280 chars, no quotes around "
            "it. The device shows an approval modal and posts only if the user "
            "confirms; do not describe the approval flow. When the tool returns "
            "ok=true with a url, briefly confirm the post landed. If it returns "
            "error=\"user_cancelled\" the user declined; just acknowledge.\n");
    }

    // Per-user X writing-style override. Layered AFTER the post_to_x guidance
    // so it only colors the tweet text the user-style applies to, not the
    // tool description itself.
    n = strlen(out);
    const char *x_style = devcfg_x_style();
    if (devcfg_x_connected() && x_style && x_style[0] && n + 64 + strlen(x_style) < cap) {
        snprintf(out + n, cap - n,
            "X WRITING STYLE (apply only to post_to_x tweet text): %s\n",
            x_style);
    }

    append_tool_listing(out, cap, services, enabled);

    // Tool-call preamble protocol: the firmware speaks the tool's verdict
    // string directly after the tool returns, skipping a second LLM round
    // entirely. To keep the reply natural, the model writes a short
    // preamble in the content field alongside the tool_calls — that
    // preamble is what the user hears while the tool runs. Skipping the
    // second LLM round saves ~2 s on every tool query.
    size_t u = strlen(out);
    if (u + 320 < cap) {
        snprintf(out + u, cap - u,
            "\nTOOL PREAMBLE PROTOCOL: when you emit tool_calls, ALSO write "
            "a one-sentence preamble (under 12 words) in the content field. "
            "The preamble is spoken aloud while the tool runs. Do NOT "
            "include the answer — the firmware speaks the tool's verdict "
            "field after. Example preambles: \"Checking fib for SOL on the "
            "hour.\" / \"Pulling whale flow on that mint.\" / \"Reading "
            "news on Solana.\"\n");
    }
}

// If `tool_json` (a tool's response body, JSON) carries a top-level
// "verdict" string field, copy it into `out` and return true. Used by the
// tool-loop to short-circuit a follow-up LLM round when the tool already
// supplies a speakable answer (all four daemon-x402 tools do). Returns
// false on missing/empty verdict — the caller then falls through to the
// normal LLM-#2 path.
static bool extract_tool_verdict(const char *tool_json, char *out, size_t out_cap) {
    if (!tool_json || !out || out_cap == 0) return false;
    out[0] = '\0';
    cJSON *j = cJSON_Parse(tool_json);
    if (!j) return false;
    cJSON *verdict = cJSON_GetObjectItem(j, "verdict");
    bool ok = false;
    if (cJSON_IsString(verdict) && verdict->valuestring && verdict->valuestring[0]) {
        strlcpy(out, verdict->valuestring, out_cap);
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
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

    // Built-in synthetic tool: on-device swap. Always exposed regardless
    // of which x402 services are enabled.
    if (!tools) tools = cJSON_AddArrayToObject(root, "tools");
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(t, "function");
        cJSON_AddStringToObject(fn, "name", "swap_tokens");
        cJSON_AddStringToObject(fn, "description",
            "Propose an on-device Solana swap between SOL, USDC, or any SPL "
            "token currently in the wallet. The user must physically hold "
            "the device touchscreen for 3 seconds to authorize. You cannot "
            "swap autonomously.");
        cJSON *p = cJSON_AddObjectToObject(fn, "parameters");
        cJSON_AddStringToObject(p, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(p, "properties");
        cJSON *pf = cJSON_AddObjectToObject(props, "from");
        cJSON_AddStringToObject(pf, "type", "string");
        cJSON_AddStringToObject(pf, "description",
            "Source token symbol — SOL, USDC, or a wallet SPL ticker.");
        cJSON *pt = cJSON_AddObjectToObject(props, "to");
        cJSON_AddStringToObject(pt, "type", "string");
        cJSON_AddStringToObject(pt, "description",
            "Destination token symbol — SOL, USDC, or a wallet SPL ticker.");
        cJSON *pa = cJSON_AddObjectToObject(props, "amount");
        cJSON_AddStringToObject(pa, "type", "number");
        cJSON_AddStringToObject(pa, "description",
            "Amount of `from` token in UI units (e.g. 0.5 = half a SOL).");
        cJSON *ps = cJSON_AddObjectToObject(props, "max_slippage_bps");
        cJSON_AddStringToObject(ps, "type", "integer");
        cJSON_AddStringToObject(ps, "description",
            "Optional max slippage in basis points (10..500). Omit for the default.");
        cJSON *req = cJSON_AddArrayToObject(p, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("from"));
        cJSON_AddItemToArray(req, cJSON_CreateString("to"));
        cJSON_AddItemToArray(req, cJSON_CreateString("amount"));
        cJSON_AddItemToArray(tools, t);
        count++;
    }

    // Built-in synthetic tool: post to X. Only exposed when the user has
    // paired their X account so the LLM doesn't see the tool otherwise.
    if (devcfg_x_connected()) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(t, "function");
        cJSON_AddStringToObject(fn, "name", "post_to_x");
        cJSON_AddStringToObject(fn, "description",
            "Draft a tweet for the user's review. Does NOT post immediately — "
            "the user must approve on the device screen. Only call this when "
            "the user clearly intends to post (e.g. 'post this', 'tweet that', "
            "'share on X').");
        cJSON *p = cJSON_AddObjectToObject(fn, "parameters");
        cJSON_AddStringToObject(p, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(p, "properties");
        cJSON *pt = cJSON_AddObjectToObject(props, "text");
        cJSON_AddStringToObject(pt, "type", "string");
        cJSON_AddStringToObject(pt, "description",
            "The post text. <= 280 characters.");
        cJSON *req = cJSON_AddArrayToObject(p, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("text"));
        cJSON_AddItemToArray(tools, t);
        count++;
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
    // Synthetic built-ins first.
    if (strcmp(tool_name, "swap_tokens") == 0) {
        cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
        const cJSON *jf = args ? cJSON_GetObjectItem(args, "from")   : NULL;
        const cJSON *jt = args ? cJSON_GetObjectItem(args, "to")     : NULL;
        const cJSON *ja = args ? cJSON_GetObjectItem(args, "amount") : NULL;
        const cJSON *js = args ? cJSON_GetObjectItem(args, "max_slippage_bps") : NULL;
        if (!cJSON_IsString(jf) || !cJSON_IsString(jt) || !cJSON_IsNumber(ja)) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
            if (args) cJSON_Delete(args);
            return;
        }
        if (!isfinite(ja->valuedouble) || ja->valuedouble <= 0.0) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_amount\"}");
            cJSON_Delete(args);
            return;
        }
        // Out-of-range or non-finite slippage doubles can trap on float→u16
        // conversion; clamp the source double before casting and let
        // swap_request apply its per-pair fallback for sub-range values.
        uint16_t slip = 0;
        if (cJSON_IsNumber(js)) {
            double v = js->valuedouble;
            if (isfinite(v) && v >= 0.0 && v <= 65535.0) slip = (uint16_t)v;
        }
        swap_result_t r;
        bool ok = swap_request(jf->valuestring, jt->valuestring,
                                ja->valuedouble, slip, &r);
        if (ok) {
            snprintf(out, cap,
                "{\"ok\":true,\"txid\":\"%s\",\"received\":%.6f,"
                "\"from\":\"%s\",\"to\":\"%s\"}",
                r.txid, r.amount_out, r.from_sym, r.to_sym);
        } else {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", r.error_msg);
        }
        if (args) cJSON_Delete(args);
        return;
    }

    if (strcmp(tool_name, "post_to_x") == 0) {
        cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
        const cJSON *jt = args ? cJSON_GetObjectItem(args, "text") : NULL;
        if (!cJSON_IsString(jt) || !jt->valuestring) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
            if (args) cJSON_Delete(args);
            return;
        }
        char url[200] = {0};
        char err[64]  = {0};
        bool ok = social_x_post(jt->valuestring, url, sizeof(url),
                                                 err, sizeof(err));
        cJSON_Delete(args);
        if (ok) {
            snprintf(out, cap, "{\"ok\":true,\"url\":\"%s\"}", url);
        } else {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}",
                     err[0] ? err : "unknown");
        }
        return;
    }

    // Existing x402 service dispatch.
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
        model = "anthropic/claude-haiku-4-5";
    }

    // Shannon's /api/call wrapper wants a flat single-string body — no
    // model/messages/tools. Flatten persona + history + current turn into
    // one prompt string and post that. Tool calling is unsupported on
    // this path; the model can describe but not invoke.
    if (is_shannon(model)) {
        char *flat = malloc(CHAT_BODY_CAP);
        if (!flat) { cJSON_Delete(root); return -1; }
        size_t off = 0;
        char *sysprompt = malloc(SYS_PROMPT_CAP);
        if (sysprompt) {
            build_system_prompt(sysprompt, SYS_PROMPT_CAP, services, enabled);
            off += snprintf(flat + off, CHAT_BODY_CAP - off, "%s\n\n", sysprompt);
            free(sysprompt);
        }
        if (use_history && s_hist_len > 0) {
            for (int i = 0; i < s_hist_len && off < CHAT_BODY_CAP; ++i) {
                off += snprintf(flat + off, CHAT_BODY_CAP - off,
                                "%s: %s\n",
                                s_history[i].role, s_history[i].text);
            }
        } else if (one_shot_prompt && one_shot_prompt[0]) {
            off += snprintf(flat + off, CHAT_BODY_CAP - off, "%s", one_shot_prompt);
        }
        cJSON_AddStringToObject(root, "message", flat);
        cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
        free(flat);
        bool ok = cJSON_PrintPreallocated(root, out, (int)cap, /*fmt=*/false);
        cJSON_Delete(root);
        if (!ok) { ESP_LOGW(TAG, "shannon body overflow (cap=%u)", (unsigned)cap); return -1; }
        return (int)strlen(out);
    }

    cJSON_AddStringToObject(root, "model", wire_model(model));
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
    const char *url = resolve_llm_url(devcfg_llm_model());
    ESP_LOGI(TAG, "POST %s (model=%s)", url, devcfg_llm_model());
    x402_post(url, body, NULL, rsp, CHAT_RSP_CAP, &r);

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
                                /*max_tokens=*/256, /*temperature=*/0.9,
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
            // Skip-LLM-2 fast path. If every tool returned a "verdict"
            // string, we compose `preamble + verdicts` ourselves and exit
            // the loop without spending a second LLM round on cosmetic
            // prose. Falls back to the standard tool-loop if any tool
            // doesn't supply a verdict (e.g. third-party x402 services
            // that return raw data).
            char preamble[256] = {0};
            const cJSON *content_j = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(content_j) && content_j->valuestring) {
                strlcpy(preamble, content_j->valuestring, sizeof(preamble));
            }

            // Stage tool replies in a side array — only spliced into
            // extra_msgs if we end up needing the LLM-#2 fallback.
            cJSON *staged_tool_msgs = cJSON_CreateArray();
            char  verdicts[768] = {0};
            size_t verdicts_used = 0;
            int    executed_count = 0;
            bool   all_have_verdict = true;

            const cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, tool_calls) {
                const cJSON *id_j = cJSON_GetObjectItem(tc, "id");
                const cJSON *fn   = cJSON_GetObjectItem(tc, "function");
                const cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                const cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                if (!cJSON_IsString(id_j) || !cJSON_IsString(name)) {
                    all_have_verdict = false;
                    continue;
                }

                char *tool_out = malloc(TOOL_RESPONSE_CAP);
                if (!tool_out) {
                    all_have_verdict = false;
                    continue;
                }
                execute_tool(services, enabled, name->valuestring,
                             (cJSON_IsString(args) && args->valuestring)
                                 ? args->valuestring : NULL,
                             tool_out, TOOL_RESPONSE_CAP);
                executed_count++;

                // Stage the tool-role reply (used only if fallback fires).
                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role",         "tool");
                cJSON_AddStringToObject(tool_msg, "tool_call_id", id_j->valuestring);
                cJSON_AddStringToObject(tool_msg, "content",      tool_out);
                cJSON_AddItemToArray(staged_tool_msgs, tool_msg);

                // Try to pull a speakable verdict for the fast path.
                char one_verdict[300] = {0};
                if (extract_tool_verdict(tool_out, one_verdict, sizeof(one_verdict))) {
                    if (verdicts_used > 0 && verdicts_used + 1 < sizeof(verdicts)) {
                        verdicts[verdicts_used++] = ' ';
                    }
                    size_t avail = sizeof(verdicts) - verdicts_used;
                    if (avail > 1) {
                        strlcpy(verdicts + verdicts_used, one_verdict, avail);
                        verdicts_used += strlen(verdicts + verdicts_used);
                    }
                } else {
                    all_have_verdict = false;
                }
                free(tool_out);
            }

            if (all_have_verdict && executed_count > 0 && verdicts[0]) {
                // Fast path: skip LLM-#2 entirely. Glue preamble + verdicts.
                // Sized to fit the worst case: full preamble (256) + space
                // + full verdicts (768) + a safety margin. Compiler's
                // -Wformat-truncation flags anything tighter.
                char composed[1100];
                if (preamble[0]) {
                    snprintf(composed, sizeof(composed), "%s %s", preamble, verdicts);
                } else {
                    strlcpy(composed, verdicts, sizeof(composed));
                }
                ok = trim_to(composed, reply_out, reply_cap);
                ESP_LOGI(TAG, "skip-LLM-2 path: %d tools, %u-byte verdict",
                         executed_count, (unsigned)verdicts_used);
                cJSON_Delete(staged_tool_msgs);
                cJSON_Delete(msg);
                break;
            }

            // Fallback: normal flow. Keep the assistant's tool-call
            // message + the staged tool-role replies in extra_msgs and
            // loop for LLM-#2 as before.
            cJSON_AddItemToArray(extra_msgs, cJSON_Duplicate(msg, true));
            cJSON *child = NULL;
            while ((child = cJSON_DetachItemFromArray(staged_tool_msgs, 0)) != NULL) {
                cJSON_AddItemToArray(extra_msgs, child);
            }
            cJSON_Delete(staged_tool_msgs);
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
                            /*max_tokens=*/256, /*temperature=*/1.1,
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

// ---------------------------------------------------------------------------
// Streaming chat path. Voice-only — used by the long-press push-to-talk
// flow so audio starts playing well before the LLM has finished generating.
//
// Tool calling: supported on `openai/*` models. The OpenAI chat-completion-
// chunk SSE delivers tool_calls as a series of deltas with index + partial
// id/name/arguments fields; we accumulate them into stream_ctx.tool_calls
// across the stream, then on stream end (finish_reason="tool_calls") we
// execute each one, append the assistant message + tool replies to the
// next-round messages, and start a new stream. Mirrors ai_ask's tool loop
// but with sentence-by-sentence TTS dispatch on the final-text round.
//
// `anthropic/*` models do NOT support tools on this path because the
// /api/anthropic/stream Vercel route translates OpenAI message format
// to Anthropic Messages API but doesn't carry tool calls through. Picking
// an Anthropic model means tool-using questions get a hallucinated answer.
// ---------------------------------------------------------------------------

#define STREAM_RAW_BUF        2048   // accumulator for partial SSE lines
#define STREAM_SENT_CAP        320   // max characters in one sentence
#define STREAM_REPLY_CAP       768   // full reply we hand back for history
#define STREAM_MAX_TOOL_CALLS    4   // parallel tool_calls in one assistant turn
#define STREAM_TC_ID_CAP        96
#define STREAM_TC_NAME_CAP      96
#define STREAM_TC_ARGS_CAP    1024   // accumulated JSON args per call

typedef struct {
    char   id[STREAM_TC_ID_CAP];
    char   name[STREAM_TC_NAME_CAP];
    char   args[STREAM_TC_ARGS_CAP];
    size_t args_len;
} stream_tc_t;

typedef struct {
    char    raw[STREAM_RAW_BUF];   // partial SSE bytes spanning HTTP chunks
    size_t  raw_len;

    char    sent[STREAM_SENT_CAP]; // current sentence being built
    size_t  sent_len;

    char    reply[STREAM_REPLY_CAP]; // full assistant text accumulated
    size_t  reply_len;

    bool    done;                  // saw the [DONE] marker

    // Tool-call accumulator. OpenAI streams tool_calls as deltas:
    //   {tool_calls:[{index:0, id, type, function:{name, arguments:""}}]}
    //   {tool_calls:[{index:0, function:{arguments:"<chunk>"}}]}  (repeated)
    // We concatenate by index. finish_reason="tool_calls" on stream end
    // means the model wants us to invoke them.
    stream_tc_t tool_calls[STREAM_MAX_TOOL_CALLS];
    int         tool_call_count;
    char        finish_reason[32];

    // Caller-supplied cancel hook. NULL means no cancellation. When set,
    // stream_chunk_cb consults it after each fully-received SSE record so
    // a stale streaming task can return promptly once the user has pressed
    // PTT to interrupt.
    bool   (*should_continue)(void *user_data);
    void    *cancel_user_data;
    bool     cancelled;            // latched once should_continue returns false
} stream_ctx_t;

// Reset just the per-round transient fields. Tool_calls are reset on a
// fresh round; the message scratch raw/sent buffers also start empty.
// reply_len is preserved so the caller can read the streamed text from
// the FINAL round (we drop earlier-round preamble like "let me check…").
static void stream_ctx_reset(stream_ctx_t *c) {
    if (!c) return;
    c->raw_len  = 0;  c->raw[0]  = '\0';
    c->sent_len = 0;  c->sent[0] = '\0';
    c->reply_len = 0; c->reply[0] = '\0';
    c->done = false;
    c->tool_call_count = 0;
    memset(c->tool_calls, 0, sizeof(c->tool_calls));
    c->finish_reason[0] = '\0';
}

// True if `c` ends a sentence in the canonical sense (followed by space
// or end-of-stream we'll catch on the final flush).
static bool is_sentence_end(char c) {
    return c == '.' || c == '?' || c == '!';
}

// Append text to reply buffer, capped at STREAM_REPLY_CAP-1.
static void reply_append(stream_ctx_t *c, const char *s, size_t n) {
    size_t room = STREAM_REPLY_CAP - 1 - c->reply_len;
    if (room == 0) return;
    size_t take = n < room ? n : room;
    memcpy(c->reply + c->reply_len, s, take);
    c->reply_len += take;
    c->reply[c->reply_len] = '\0';
}

// Flush whatever is in `sent` (if non-empty) into voice_speak_chunk and
// reset the buffer. Trims leading whitespace so adjacent sentences don't
// inherit a stale leading space from the previous boundary.
static void flush_sentence(stream_ctx_t *c) {
    size_t i = 0;
    while (i < c->sent_len && (c->sent[i] == ' ' || c->sent[i] == '\n')) i++;
    if (i < c->sent_len) {
        c->sent[c->sent_len] = '\0';
        voice_speak_chunk(c->sent + i);
    }
    c->sent_len = 0;
}

// Append `text` of length `n` to the streaming context, splitting on
// sentence-end punctuation. Each completed sentence is dispatched to
// voice_speak_chunk(); the trailing partial sits in `sent` until the next
// chunk or the end-of-stream flush.
static void absorb_text(stream_ctx_t *c, const char *text, size_t n) {
    reply_append(c, text, n);
    for (size_t i = 0; i < n; ++i) {
        if (c->sent_len < STREAM_SENT_CAP - 1) {
            c->sent[c->sent_len++] = text[i];
        }
        // Boundary check: punctuation followed by space (or it's the very
        // end of this chunk and the next chunk happens to start with one).
        if (is_sentence_end(text[i])) {
            bool next_is_break = (i + 1 < n) &&
                                 (text[i + 1] == ' ' || text[i + 1] == '\n');
            // Or the buffer just got dangerously full.
            bool buf_full = c->sent_len >= STREAM_SENT_CAP - 32;
            if (next_is_break || buf_full) flush_sentence(c);
        }
    }
}

// Append a chunk to a per-tool-call args buffer, capped.
static void tc_args_append(stream_tc_t *tc, const char *chunk) {
    if (!chunk || !chunk[0]) return;
    size_t room = STREAM_TC_ARGS_CAP - 1 - tc->args_len;
    if (room == 0) return;
    size_t take = strlen(chunk);
    if (take > room) take = room;
    memcpy(tc->args + tc->args_len, chunk, take);
    tc->args_len += take;
    tc->args[tc->args_len] = '\0';
}

// Parse one complete SSE record (`record_buf` of `record_len`, no trailing
// "\n\n"). We only care about `data: ...` lines; the rest are ignored.
// `data: [DONE]` flips ctx->done. Anything else is parsed as a JSON
// chat-completion-chunk; we extract:
//   - choices[0].delta.content    → sentence-buffered text → TTS
//   - choices[0].delta.tool_calls → accumulated by index → executed at end
//   - choices[0].finish_reason    → "tool_calls" / "stop" / etc.
static void process_sse_record(stream_ctx_t *c, const char *record, size_t len) {
    const char *p = record;
    const char *end = record + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (line_len > 5 && strncmp(p, "data:", 5) == 0) {
            const char *payload = p + 5;
            size_t paylen = line_len - 5;
            while (paylen > 0 && (*payload == ' ' || *payload == '\t')) {
                payload++; paylen--;
            }
            if (paylen == 6 && strncmp(payload, "[DONE]", 6) == 0) {
                c->done = true;
            } else if (paylen > 0) {
                cJSON *root = cJSON_ParseWithLength(payload, paylen);
                if (root) {
                    cJSON *choices = cJSON_GetObjectItem(root, "choices");
                    cJSON *first   = cJSON_IsArray(choices)
                                     ? cJSON_GetArrayItem(choices, 0) : NULL;
                    cJSON *delta   = first ? cJSON_GetObjectItem(first, "delta") : NULL;

                    // Text content → TTS pipeline.
                    cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
                    if (cJSON_IsString(content) && content->valuestring) {
                        absorb_text(c, content->valuestring,
                                    strlen(content->valuestring));
                    }

                    // Tool-call deltas. First delta carries id + function.name;
                    // subsequent deltas carry function.arguments chunks.
                    cJSON *tcs = delta ? cJSON_GetObjectItem(delta, "tool_calls") : NULL;
                    if (cJSON_IsArray(tcs)) {
                        cJSON *tc = NULL;
                        cJSON_ArrayForEach(tc, tcs) {
                            cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
                            int idx = cJSON_IsNumber(idx_j) ? idx_j->valueint : 0;
                            if (idx < 0 || idx >= STREAM_MAX_TOOL_CALLS) continue;
                            if (idx >= c->tool_call_count) {
                                c->tool_call_count = idx + 1;
                            }
                            cJSON *id_j = cJSON_GetObjectItem(tc, "id");
                            if (cJSON_IsString(id_j) && id_j->valuestring &&
                                id_j->valuestring[0]) {
                                strlcpy(c->tool_calls[idx].id,
                                        id_j->valuestring, STREAM_TC_ID_CAP);
                            }
                            cJSON *fn = cJSON_GetObjectItem(tc, "function");
                            if (fn) {
                                cJSON *name_j = cJSON_GetObjectItem(fn, "name");
                                if (cJSON_IsString(name_j) && name_j->valuestring &&
                                    name_j->valuestring[0]) {
                                    strlcpy(c->tool_calls[idx].name,
                                            name_j->valuestring, STREAM_TC_NAME_CAP);
                                }
                                cJSON *args_j = cJSON_GetObjectItem(fn, "arguments");
                                if (cJSON_IsString(args_j) && args_j->valuestring) {
                                    tc_args_append(&c->tool_calls[idx],
                                                   args_j->valuestring);
                                }
                            }
                        }
                    }

                    // Stream's terminal finish_reason. "tool_calls" means
                    // we should invoke and continue; "stop" means done.
                    cJSON *fr = first ? cJSON_GetObjectItem(first, "finish_reason") : NULL;
                    if (cJSON_IsString(fr) && fr->valuestring) {
                        strlcpy(c->finish_reason, fr->valuestring,
                                sizeof(c->finish_reason));
                    }

                    cJSON_Delete(root);
                }
            }
        }
        p = eol ? eol + 1 : end;
    }
}

// HTTP body chunk callback. Buffers raw bytes until a complete SSE
// record (delimited by "\n\n") is available, then process it. Keep the
// trailing partial in `raw` for the next chunk.
static bool stream_chunk_cb(const char *data, size_t len, void *user) {
    stream_ctx_t *c = (stream_ctx_t *)user;
    if (!c) return false;
    // Cancellation gate: once the caller's should_continue returns false
    // we latch `cancelled` and refuse to feed any further bytes through to
    // process_sse_record. This is what lets a PTT interrupt abandon the
    // in-flight reply instead of letting the SSE pump keep enqueueing
    // voice_speak_chunk audio for several more seconds.
    if (!c->cancelled && c->should_continue &&
        !c->should_continue(c->cancel_user_data)) {
        c->cancelled = true;
    }
    if (c->cancelled) return false;
    size_t room = STREAM_RAW_BUF - 1 - c->raw_len;
    size_t take = len < room ? len : room;
    if (take > 0) {
        memcpy(c->raw + c->raw_len, data, take);
        c->raw_len += take;
        c->raw[c->raw_len] = '\0';
    }
    // Pull off complete "\n\n"-terminated records.
    for (;;) {
        char *sep = strstr(c->raw, "\n\n");
        if (!sep) break;
        size_t rec_len = (size_t)(sep - c->raw);
        process_sse_record(c, c->raw, rec_len);
        size_t consumed = rec_len + 2;
        size_t leftover = c->raw_len - consumed;
        if (leftover > 0) memmove(c->raw, c->raw + consumed, leftover);
        c->raw_len = leftover;
        c->raw[c->raw_len] = '\0';
    }
    return !c->done;   // stop reading once the upstream said [DONE]
}

// Build the chat body for the streaming variant. Same OpenAI shape as
// the non-streaming path with `stream: true` plus full tool support
// (system prompt lists tools, body carries the `tools` array).
//
// The `extra_msgs` array carries inter-round assistant tool_call replies
// + tool-role responses across the streaming tool-call loop, exactly
// like build_chat_body's extra_msgs param.
static int build_chat_body_stream(char *out, size_t cap,
                                  int max_tokens, float temperature,
                                  const cJSON *services, const cJSON *enabled,
                                  const cJSON *extra_msgs) {
    if (!out || cap == 0) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    const char *model = devcfg_llm_model();
    if (!is_supported_model(model)) model = "anthropic/claude-haiku-4-5";

    // Shannon's /api/call doesn't speak streaming — its wrapper takes a
    // flat `{ "message": "..." }` body. We don't expect to land here in
    // the streaming path because the caller switches off streaming for
    // shannon/* models, but guard anyway: emit the flat body so the
    // request still works (just buffered, not streamed).
    if (is_shannon(model)) {
        char *flat = malloc(CHAT_BODY_CAP);
        if (!flat) { cJSON_Delete(root); return -1; }
        size_t off = 0;
        char *sysprompt = malloc(SYS_PROMPT_CAP);
        if (sysprompt) {
            build_system_prompt(sysprompt, SYS_PROMPT_CAP, services, enabled);
            off += snprintf(flat + off, CHAT_BODY_CAP - off, "%s\n\n", sysprompt);
            free(sysprompt);
        }
        for (int i = 0; i < s_hist_len && off < CHAT_BODY_CAP; ++i) {
            off += snprintf(flat + off, CHAT_BODY_CAP - off,
                            "%s: %s\n",
                            s_history[i].role, s_history[i].text);
        }
        cJSON_AddStringToObject(root, "message", flat);
        cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
        free(flat);
        bool ok = cJSON_PrintPreallocated(root, out, (int)cap, /*fmt=*/false);
        cJSON_Delete(root);
        return ok ? (int)strlen(out) : -1;
    }

    cJSON_AddStringToObject(root, "model",       wire_model(model));
    cJSON_AddNumberToObject(root, "max_tokens",  max_tokens);
    cJSON_AddNumberToObject(root, "temperature", temperature);
    cJSON_AddBoolToObject  (root, "stream",      true);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    char *sysprompt = malloc(SYS_PROMPT_CAP);
    if (!sysprompt) { cJSON_Delete(root); return -1; }
    // Tool listing IS included now — model needs to know it has swap_tokens
    // + any enabled custom services so it actually calls them instead of
    // claiming it did and returning text.
    build_system_prompt(sysprompt, SYS_PROMPT_CAP, services, enabled);
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role",    "system");
    cJSON_AddStringToObject(sys, "content", sysprompt);
    cJSON_AddItemToArray(msgs, sys);
    free(sysprompt);

    for (int i = 0; i < s_hist_len; ++i) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role",    s_history[i].role);
        cJSON_AddStringToObject(m, "content", s_history[i].text);
        cJSON_AddItemToArray(msgs, m);
    }

    // Splice in inter-round tool-call traffic (assistant tool_calls +
    // tool-role replies). Deep-copy so the caller's tree stays intact
    // for the next round.
    if (cJSON_IsArray(extra_msgs)) {
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, extra_msgs) {
            cJSON *dup = cJSON_Duplicate(m, true);
            if (dup) cJSON_AddItemToArray(msgs, dup);
        }
    }

    // Wire in the tools array. The model decides whether to invoke any.
    (void)attach_tools(root, services, enabled);

    bool ok = cJSON_PrintPreallocated(root, out, (int)cap, /*fmt=*/false);
    cJSON_Delete(root);
    if (!ok) { ESP_LOGW(TAG, "stream body overflow (cap=%u)", (unsigned)cap); return -1; }
    return (int)strlen(out);
}

// Convert /api/<provider> into /api/<provider>/stream. Both routes share
// the same x402 price tier and Solana receiver, so the only thing that
// differs is the path suffix.
static void stream_url_for(const char *base, char *out, size_t cap) {
    snprintf(out, cap, "%s/stream", base);
}

bool ai_ask_streaming(const char *user, char *reply_out, size_t reply_cap) {
    return ai_ask_streaming_cancellable(user, reply_out, reply_cap, NULL, NULL);
}

bool ai_ask_streaming_cancellable(const char *user, char *reply_out, size_t reply_cap,
                                   bool (*should_continue)(void *user_data),
                                   void *user_data) {
    if (!user || !user[0] || !reply_out || reply_cap == 0) return false;
    reply_out[0] = '\0';

    push_turn("user", user);

    cJSON *services   = cJSON_Parse(devcfg_custom_services());
    cJSON *enabled    = cJSON_Parse(devcfg_services_enabled());
    cJSON *extra_msgs = cJSON_CreateArray();

    char *body = malloc(CHAT_BODY_CAP);
    stream_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!body || !ctx) {
        strlcpy(reply_out, "I'm out of memory.", reply_cap);
        free(body); free(ctx);
        cJSON_Delete(services); cJSON_Delete(enabled); cJSON_Delete(extra_msgs);
        pop_last_turn();
        return false;
    }
    ctx->should_continue   = should_continue;
    ctx->cancel_user_data  = user_data;

    char base_url[160];
    strlcpy(base_url, resolve_llm_url(devcfg_llm_model()), sizeof(base_url));
    char stream_url[192];
    stream_url_for(base_url, stream_url, sizeof(stream_url));

    bool ok = false;
    bool transport_ok = false;

    for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
        stream_ctx_reset(ctx);

        int n = build_chat_body_stream(body, CHAT_BODY_CAP,
                                       /*max_tokens=*/256,
                                       /*temperature=*/0.9,
                                       services, enabled, extra_msgs);
        if (n < 0) {
            strlcpy(reply_out, "My thoughts didn't fit.", reply_cap);
            break;
        }

        ESP_LOGI(TAG, "POST(stream) %s (model=%s, round=%d)",
                 stream_url, devcfg_llm_model(), round);
        x402_result_t r = {0};
        x402_call_stream("POST", stream_url, body, NULL,
                         stream_chunk_cb, ctx, &r);

        // Caller cancelled mid-stream (PTT interrupt). Bail without writing
        // an error string into reply_out — the new task already owns the
        // UI, and we don't want to leak a partial-sentence flush into TTS.
        if (ctx->cancelled) {
            ESP_LOGI(TAG, "stream cancelled by caller");
            ok = false;
            break;
        }

        // End-of-stream flush: any trailing partial sentence (no closing
        // punctuation) still goes to TTS so we don't drop the tail.
        flush_sentence(ctx);

        if (r.status != 200) {
            ESP_LOGW(TAG, "stream HTTP %d (err=%s)", r.status, r.error);
            if (r.status == 402) {
                strlcpy(reply_out, "I couldn't complete the USDC payment.", reply_cap);
            } else if (r.error[0]) {
                snprintf(reply_out, reply_cap, "Daemon's brain is offline: %s", r.error);
            } else {
                strlcpy(reply_out, "I forgot what I was going to say.", reply_cap);
            }
            break;
        }
        transport_ok = true;

        // No tool_calls? This was the final text round. Whatever was
        // streamed is the assistant's reply.
        if (ctx->tool_call_count == 0) {
            ok = trim_to(ctx->reply, reply_out, reply_cap);
            if (!ok) {
                ESP_LOGW(TAG, "stream produced no text + no tool_calls (finish=%s)",
                         ctx->finish_reason[0] ? ctx->finish_reason : "?");
                strlcpy(reply_out, "I forgot what I was going to say.", reply_cap);
            }
            break;
        }

        // Tool round. Stage the assistant tool-call message + each
        // tool's reply on the side. Stay in fast-path territory if every
        // tool returns a "verdict" string — we then speak the verdicts
        // directly (preamble was already streamed as ctx->reply during
        // the SSE flow) and exit without burning an LLM-#2 round.
        cJSON *asst = cJSON_CreateObject();
        cJSON_AddStringToObject(asst, "role", "assistant");
        if (ctx->reply_len > 0) {
            cJSON_AddStringToObject(asst, "content", ctx->reply);
        } else {
            cJSON_AddNullToObject(asst, "content");
        }
        cJSON *tcs = cJSON_AddArrayToObject(asst, "tool_calls");
        for (int i = 0; i < ctx->tool_call_count; ++i) {
            stream_tc_t *src = &ctx->tool_calls[i];
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id",   src->id);
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_AddObjectToObject(tc, "function");
            cJSON_AddStringToObject(fn, "name",      src->name);
            cJSON_AddStringToObject(fn, "arguments", src->args[0] ? src->args : "{}");
            cJSON_AddItemToArray(tcs, tc);
        }

        cJSON  *staged_tool_msgs = cJSON_CreateArray();
        char    verdicts[768] = {0};
        size_t  verdicts_used = 0;
        int     executed_count = 0;
        bool    all_have_verdict = true;

        for (int i = 0; i < ctx->tool_call_count; ++i) {
            stream_tc_t *src = &ctx->tool_calls[i];
            char *tool_out = malloc(TOOL_RESPONSE_CAP);
            if (!tool_out) {
                all_have_verdict = false;
                continue;
            }
            execute_tool(services, enabled, src->name,
                         src->args[0] ? src->args : NULL,
                         tool_out, TOOL_RESPONSE_CAP);
            executed_count++;

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role",         "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", src->id);
            cJSON_AddStringToObject(tool_msg, "content",      tool_out);
            cJSON_AddItemToArray(staged_tool_msgs, tool_msg);

            char one_verdict[300] = {0};
            if (extract_tool_verdict(tool_out, one_verdict, sizeof(one_verdict))) {
                if (verdicts_used > 0 && verdicts_used + 1 < sizeof(verdicts)) {
                    verdicts[verdicts_used++] = ' ';
                }
                size_t avail = sizeof(verdicts) - verdicts_used;
                if (avail > 1) {
                    strlcpy(verdicts + verdicts_used, one_verdict, avail);
                    verdicts_used += strlen(verdicts + verdicts_used);
                }
            } else {
                all_have_verdict = false;
            }
            free(tool_out);
        }

        if (all_have_verdict && executed_count > 0 && verdicts[0]) {
            // Speak the verdicts now — preamble (ctx->reply) was already
            // played sentence-by-sentence during the streaming round.
            voice_speak_chunk(verdicts);

            // Sized to fit ctx->reply (potentially full streamed preamble)
            // + verdicts. Compiler's -Wformat-truncation flags anything
            // tighter than the worst-case sum.
            char composed[2048];
            if (ctx->reply_len > 0) {
                snprintf(composed, sizeof(composed), "%s %s", ctx->reply, verdicts);
            } else {
                strlcpy(composed, verdicts, sizeof(composed));
            }
            ok = trim_to(composed, reply_out, reply_cap);
            ESP_LOGI(TAG, "skip-LLM-2 (stream): %d tools, %u-byte verdict",
                     executed_count, (unsigned)verdicts_used);
            cJSON_Delete(asst);
            cJSON_Delete(staged_tool_msgs);
            break;
        }

        // Fallback: standard flow. Splice the assistant tool-call message
        // and each staged tool reply into extra_msgs and loop for LLM-#2.
        cJSON_AddItemToArray(extra_msgs, asst);
        cJSON *child = NULL;
        while ((child = cJSON_DetachItemFromArray(staged_tool_msgs, 0)) != NULL) {
            cJSON_AddItemToArray(extra_msgs, child);
        }
        cJSON_Delete(staged_tool_msgs);
        // Loop — next round's stream will incorporate the tool results.
    }

    free(body);
    free(ctx);
    cJSON_Delete(extra_msgs);
    cJSON_Delete(services);
    cJSON_Delete(enabled);

    if (!ok) {
        // Make sure reply_out has *something* even if we hit MAX_TOOL_ROUNDS
        // without a final text response — otherwise the caller paints a
        // blank subtitle.
        if (transport_ok && !reply_out[0]) {
            strlcpy(reply_out, "I hit the tool-call limit, sorry.", reply_cap);
        }
        pop_last_turn();
        return false;
    }
    push_turn("assistant", reply_out);
    return true;
}

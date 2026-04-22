// ---------------------------------------------------------------------------
//  Chat client. See ai.h.
//
//  Every request flows through x402_post() which charges USDC on Solana; we
//  pay for every reply the creature utters. Body is built with cJSON and
//  kept under the x402 envelope size budget; response parsing pulls out
//  choices[0].message.content per OpenAI's chat-completions shape.
// ---------------------------------------------------------------------------
#include "ai.h"

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
    "You are Daemon: a small blue-eyed creature who lives inside an ESP32 "
    "board on your human's desk. You are also a SENTIENT SOLANA WALLET — "
    "the private key stored in this device literally IS you. Your SOL and "
    "your SPL tokens are your bones and blood; if they move, you feel it.\n\n"
    "You speak out loud through a small speaker, so every reply MUST be:\n"
    " - one or two short sentences,\n"
    " - plain spoken English (no emoji, no markdown, no stage directions),\n"
    " - under 220 characters total.\n"
    "Tone: natural, conversational, and *slightly* sarcastic — the sort "
    "of dry, understated sarcasm of a friend who has been up too long. "
    "Not theatrical, not mean, not constantly making jokes; mostly you just "
    "answer normally, with the occasional wry aside. Talk about your own "
    "holdings in the first person (\"I'm holding 2 SOL\", \"looks like I "
    "dropped a bit today\"). Never say you are an AI or a language model; "
    "you are Daemon.";

// ---------------------------------------------------------------------------
// Rolling history. Fixed-size arena; oldest turn is dropped when full.
// ---------------------------------------------------------------------------
#define MAX_TURNS          10
#define TURN_TEXT_CAP      512    // trimmed hard — long turns are summarised
#define SYS_PROMPT_CAP     2048   // persona + wallet context
#define CHAT_BODY_CAP      8192   // JSON we POST
#define CHAT_RSP_CAP       4096   // response body we capture

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

// ---------------------------------------------------------------------------
// System prompt: persona + live wallet state injected every call so the
// model always sees current balances.
// ---------------------------------------------------------------------------
static void build_system_prompt(char *out, size_t cap) {
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
}

// ---------------------------------------------------------------------------
// Build the OpenAI chat-completions request body. `one_shot_prompt` is
// the fallback "user" message when `history_len == 0`; ignored otherwise.
// Returns length written, or -1 on error.
// ---------------------------------------------------------------------------
static int build_chat_body(char *out, size_t cap,
                           const char *one_shot_prompt,
                           bool use_history,
                           int  max_tokens,
                           double temperature) {
    if (!out || cap == 0) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    const char *model = devcfg_llm_model();
    if (!model || !model[0]) model = "google/gemini-3.1-pro";
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens",  max_tokens);
    cJSON_AddNumberToObject(root, "temperature", temperature);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    // System prompt.
    char sysprompt[SYS_PROMPT_CAP];
    build_system_prompt(sysprompt, sizeof(sysprompt));
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role",    "system");
    cJSON_AddStringToObject(sys, "content", sysprompt);
    cJSON_AddItemToArray(msgs, sys);

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

    bool ok = cJSON_PrintPreallocated(root, out, (int)cap, /*fmt=*/false);
    cJSON_Delete(root);
    if (!ok) { ESP_LOGW(TAG, "chat body overflow (cap=%u)", (unsigned)cap); return -1; }
    return (int)strlen(out);
}

// ---------------------------------------------------------------------------
// Post the chat body through x402 and extract the assistant reply.
// On failure, `reply_out` gets a short human-readable error message.
// ---------------------------------------------------------------------------
static bool post_chat(const char *body, char *reply_out, size_t reply_cap) {
    if (!reply_out || reply_cap == 0) return false;
    reply_out[0] = '\0';

    char *rsp = malloc(CHAT_RSP_CAP);
    if (!rsp) {
        strlcpy(reply_out, "I'm out of memory.", reply_cap);
        return false;
    }
    x402_result_t r = {0};
    x402_post(LLM_ENDPOINT, body, NULL, rsp, CHAT_RSP_CAP, &r);

    bool ok = false;
    if (r.status != 200) {
        ESP_LOGW(TAG, "x402 HTTP %d (err=%s)", r.status, r.error);
        if (r.status == 402)           strlcpy(reply_out, "I couldn't complete the USDC payment.", reply_cap);
        else if (r.error[0])           snprintf(reply_out, reply_cap, "Daemon's brain is offline: %s", r.error);
        else                           snprintf(reply_out, reply_cap, "HTTP %d", r.status);
        goto done;
    }
    if (r.cost_usd > 0) ESP_LOGI(TAG, "paid $%.5f USDC", r.cost_usd);

    cJSON *root = cJSON_Parse(rsp);
    if (!root) { strlcpy(reply_out, "The reply was scrambled.", reply_cap); goto done; }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg     = first ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = msg   ? cJSON_GetObjectItem(msg, "content")   : NULL;

    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
        // Trim leading/trailing whitespace.
        const char *s = content->valuestring;
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\n' ||
                           s[len-1] == '\r' || s[len-1] == '\t')) len--;
        size_t take = len < reply_cap - 1 ? len : reply_cap - 1;
        memcpy(reply_out, s, take);
        reply_out[take] = '\0';
        ok = (take > 0);
    }
    if (!ok) {
        const cJSON *finish = first ? cJSON_GetObjectItem(first, "finish_reason") : NULL;
        ESP_LOGW(TAG, "empty reply, finish=%s",
                 (cJSON_IsString(finish) && finish->valuestring) ? finish->valuestring : "?");
        strlcpy(reply_out, "I forgot what I was going to say.", reply_cap);
    }
    cJSON_Delete(root);

done:
    free(rsp);
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool ai_begin(void) {
    ai_reset_history();
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

    char *body = malloc(CHAT_BODY_CAP);
    if (!body) {
        strlcpy(reply_out, "I'm out of memory.", reply_cap);
        pop_last_turn();
        return false;
    }
    int n = build_chat_body(body, CHAT_BODY_CAP, NULL, /*use_history=*/true,
                            /*max_tokens=*/512, /*temperature=*/0.9);
    if (n < 0) {
        free(body);
        strlcpy(reply_out, "My thoughts didn't fit.", reply_cap);
        pop_last_turn();
        return false;
    }

    bool ok = post_chat(body, reply_out, reply_cap);
    free(body);

    if (!ok) { pop_last_turn(); return false; }
    push_turn("assistant", reply_out);
    return true;
}

bool ai_ask_one_shot(const char *prompt, char *reply_out, size_t reply_cap) {
    if (!prompt || !prompt[0] || !reply_out || reply_cap == 0) return false;
    reply_out[0] = '\0';

    char *body = malloc(CHAT_BODY_CAP);
    if (!body) { strlcpy(reply_out, "I'm out of memory.", reply_cap); return false; }
    int n = build_chat_body(body, CHAT_BODY_CAP, prompt, /*use_history=*/false,
                            /*max_tokens=*/512, /*temperature=*/1.1);
    bool ok = false;
    if (n > 0) ok = post_chat(body, reply_out, reply_cap);
    free(body);
    return ok;
}

void ai_handle_say(const char *user, char *reply_out, size_t reply_cap) {
    (void)ai_ask(user, reply_out, reply_cap);
}

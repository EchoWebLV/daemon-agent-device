// ---------------------------------------------------------------------------
// payapi.c — pay.sh dispatcher
// ---------------------------------------------------------------------------
#include "payapi.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "devcfg.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pay_confirm_screen.h"
#include "voice.h"
#include "wifi_sta.h"

extern volatile uint32_t s_ptt_gen;  // declared in creature_screen.c

static const char *TAG = "payapi";

// ---------------------------------------------------------------------------
// Catalog types
// ---------------------------------------------------------------------------

typedef struct payapi_provider {
    char    *fqn;
    char    *title;
    char    *service_url;
    uint32_t min_price_usd_cents;
    uint32_t max_price_usd_cents;
    bool     has_free_tier;
} payapi_provider_t;

typedef struct {
    payapi_provider_t *items;
    size_t             count;
    int64_t            synced_at;   // time(NULL); 0 if never
} payapi_catalog_t;

// ---------------------------------------------------------------------------
// Tool registry types
// ---------------------------------------------------------------------------

#define PAYAPI_OPENAPI_CAP   (256 * 1024)   // up to ~250 KB raw spec
#define PAYAPI_TOOL_NAME_LEN 64

typedef struct {
    char     name[PAYAPI_TOOL_NAME_LEN]; // pay_xxx_yyy_aaaa
    char    *fqn;                        // strdup, owned
    char    *service_url;                // strdup, owned
    char    *method;                     // strdup, owned (GET/POST/...)
    char    *path;                       // strdup, owned (template)
    char    *description;                // strdup, owned (LLM-facing)
    char    *params_schema_json;         // serialized JSON object string
    uint32_t price_usd_max_cents;        // ceiling from catalog
} payapi_tool_t;

typedef struct {
    char    *fqn;
    bool     loaded;
    int      tool_count;
    char     last_error[64];
    int64_t  tried_at;
} payapi_provider_status_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_catalog_mutex = NULL;
static payapi_catalog_t  s_catalog       = {0};
static bool              s_initialized   = false;

static SemaphoreHandle_t          s_tools_mutex = NULL;
static payapi_tool_t             *s_tools       = NULL;
static size_t                     s_tool_count  = 0;
static size_t                     s_tool_cap    = 0;
static payapi_provider_status_t  *s_status      = NULL;
static size_t                     s_status_cnt  = 0;
static size_t                     s_status_cap  = 0;

// ---------------------------------------------------------------------------
// HTTP GET helper — PSRAM-backed, NUL-terminated, caller frees
// ---------------------------------------------------------------------------

#define PAYAPI_CATALOG_URL "https://pay.sh/api/catalog"
#define PAYAPI_CATALOG_CAP 65536

typedef struct { char *buf; size_t cap; size_t len; } get_body_t;

static esp_err_t on_get_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    get_body_t *g = (get_body_t *)evt->user_data;
    if (!g || !g->buf || g->cap == 0) return ESP_OK;
    size_t room = g->cap - 1 - g->len;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(g->buf + g->len, evt->data, take);
    g->len += take;
    g->buf[g->len] = '\0';
    return ESP_OK;
}

// Returns PSRAM-allocated, NUL-terminated response body, or NULL on failure.
static char *http_get_psram(const char *url, size_t cap) {
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "http_get_psram: no wifi");
        return NULL;
    }

    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "http_get_psram: PSRAM alloc failed (%u bytes)", (unsigned)cap);
        return NULL;
    }
    buf[0] = '\0';

    get_body_t gb = { .buf = buf, .cap = cap, .len = 0 };

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_get_event,
        .user_data         = &gb,
        .timeout_ms        = 8000,
    };

    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        ESP_LOGE(TAG, "http_get_psram: client init failed");
        free(buf);
        return NULL;
    }

    esp_err_t err  = esp_http_client_perform(http);
    int       code = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http_get_psram: perform error: %s", esp_err_to_name(err));
        free(buf);
        return NULL;
    }
    if (code < 200 || code >= 300) {
        ESP_LOGW(TAG, "http_get_psram: HTTP %d from %s", code, url);
        free(buf);
        return NULL;
    }

    if (gb.len == gb.cap - 1) {
        ESP_LOGW(TAG, "http_get_psram: response truncated at %u bytes (URL: %s)",
                 (unsigned)gb.len, url);
    }

    return buf;
}

// ---------------------------------------------------------------------------
// Catalog parser helpers
// ---------------------------------------------------------------------------

// Convert USD double to cents with rounding, clamped to UINT32_MAX.
static uint32_t usd_to_cents(double v) {
    if (v <= 0.0) return 0;
    if (v < 0.005) return 1;       // sub-cent: round up to 1¢ minimum
    double c = v * 100.0 + 0.5;
    if (c >= (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)c;
}

// Parse `providers` array from raw JSON. Returns a heap_caps_malloc'd array
// (PSRAM) of payapi_provider_t, writes count into *out_count. Returns NULL
// on empty/failure.
static payapi_provider_t *parse_catalog(const char *json, size_t *out_count) {
    *out_count = 0;
    if (!json || !json[0]) return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "catalog: JSON parse failed");
        return NULL;
    }

    cJSON *providers = cJSON_GetObjectItem(root, "providers");
    if (!cJSON_IsArray(providers)) {
        ESP_LOGW(TAG, "catalog: no providers array");
        cJSON_Delete(root);
        return NULL;
    }

    int n = cJSON_GetArraySize(providers);
    if (n <= 0) {
        cJSON_Delete(root);
        return NULL;
    }

    payapi_provider_t *items = (payapi_provider_t *)heap_caps_calloc(
        (size_t)n, sizeof(payapi_provider_t), MALLOC_CAP_SPIRAM);
    if (!items) {
        ESP_LOGE(TAG, "catalog: PSRAM alloc for %d items failed", n);
        cJSON_Delete(root);
        return NULL;
    }

    size_t accepted = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, providers) {
        cJSON *fqn_j         = cJSON_GetObjectItem(it, "fqn");
        cJSON *title_j       = cJSON_GetObjectItem(it, "title");
        cJSON *svc_j         = cJSON_GetObjectItem(it, "service_url");
        cJSON *min_j         = cJSON_GetObjectItem(it, "min_price_usd");
        cJSON *max_j         = cJSON_GetObjectItem(it, "max_price_usd");
        cJSON *free_j        = cJSON_GetObjectItem(it, "has_free_tier");

        // Skip rows missing required fields.
        if (!cJSON_IsString(fqn_j) || !fqn_j->valuestring || !fqn_j->valuestring[0])
            continue;
        if (!cJSON_IsString(svc_j) || !svc_j->valuestring || !svc_j->valuestring[0])
            continue;

        payapi_provider_t *p = &items[accepted];
        p->fqn         = strdup(fqn_j->valuestring);
        p->title       = (cJSON_IsString(title_j) && title_j->valuestring)
                             ? strdup(title_j->valuestring) : strdup("");
        p->service_url = strdup(svc_j->valuestring);
        p->min_price_usd_cents = cJSON_IsNumber(min_j) ? usd_to_cents(min_j->valuedouble) : 0;
        p->max_price_usd_cents = cJSON_IsNumber(max_j) ? usd_to_cents(max_j->valuedouble) : 0;
        p->has_free_tier       = cJSON_IsTrue(free_j);

        if (!p->fqn || !p->title || !p->service_url) {
            // strdup OOM — drop this row cleanly.
            free(p->fqn);
            free(p->title);
            free(p->service_url);
            memset(p, 0, sizeof(*p));
            continue;
        }
        accepted++;
    }

    cJSON_Delete(root);

    if (accepted == 0) {
        free(items);
        return NULL;
    }

    *out_count = accepted;
    return items;
}

// Free all strdup'd strings inside items[], then free the array itself.
static void free_catalog_items(payapi_provider_t *items, size_t count) {
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].fqn);
        free(items[i].title);
        free(items[i].service_url);
    }
    free(items);
}

// ---------------------------------------------------------------------------
// Public API — catalog refresh
// ---------------------------------------------------------------------------

bool payapi_refresh_catalog(void) {
    ESP_LOGI(TAG, "payapi_refresh_catalog: fetching %s", PAYAPI_CATALOG_URL);

    char *raw = http_get_psram(PAYAPI_CATALOG_URL, PAYAPI_CATALOG_CAP);
    if (!raw) {
        ESP_LOGW(TAG, "payapi_refresh_catalog: GET failed");
        return false;
    }

    size_t count = 0;
    payapi_provider_t *items = parse_catalog(raw, &count);
    free(raw);

    if (!items) {
        ESP_LOGW(TAG, "payapi_refresh_catalog: parse yielded 0 providers");
        return false;
    }

    int64_t now = (int64_t)time(NULL);

    // Atomic swap under mutex.
    if (s_catalog_mutex && xSemaphoreTake(s_catalog_mutex, portMAX_DELAY) == pdTRUE) {
        payapi_provider_t *old_items = s_catalog.items;
        size_t             old_count = s_catalog.count;

        s_catalog.items     = items;
        s_catalog.count     = count;
        s_catalog.synced_at = now;

        xSemaphoreGive(s_catalog_mutex);

        free_catalog_items(old_items, old_count);
    } else {
        // No mutex yet (shouldn't happen after init, but be safe).
        free_catalog_items(s_catalog.items, s_catalog.count);
        s_catalog.items     = items;
        s_catalog.count     = count;
        s_catalog.synced_at = now;
    }

    ESP_LOGI(TAG, "payapi_refresh_catalog: %u providers cached", (unsigned)count);
    return true;
}

// ---------------------------------------------------------------------------
// Tool registry helpers — must be called under s_tools_mutex
// ---------------------------------------------------------------------------

// Free all strdup'd strings in one tool entry (does NOT free the entry itself).
// All fields are freed with plain free() even if allocated via heap_caps_malloc
// (PSRAM) — ESP-IDF's unified heap dispatcher routes the call correctly. Do NOT
// replace with heap_caps_free; it is unnecessary and would break internal-RAM strings.
static void tool_free_strings(payapi_tool_t *t) {
    if (!t) return;
    free(t->fqn);
    free(t->service_url);
    free(t->method);
    free(t->path);
    free(t->description);
    free(t->params_schema_json);
    memset(t, 0, sizeof(*t));
}

// Remove all tools whose fqn matches. Compacts the array in place.
static void tools_drop_provider_locked(const char *fqn) {
    if (!fqn || !s_tools) return;
    size_t write = 0;
    for (size_t i = 0; i < s_tool_count; i++) {
        if (s_tools[i].fqn && strcmp(s_tools[i].fqn, fqn) == 0) {
            tool_free_strings(&s_tools[i]);
        } else {
            if (write != i) s_tools[write] = s_tools[i];
            write++;
        }
    }
    s_tool_count = write;
}

// Append a fully-populated tool to s_tools[]. Grows via PSRAM realloc.
static bool tools_append_locked(const payapi_tool_t *t) {
    if (s_tool_count >= s_tool_cap) {
        size_t new_cap = s_tool_cap ? s_tool_cap * 2 : 16;
        payapi_tool_t *nb = (payapi_tool_t *)heap_caps_realloc(
            s_tools, new_cap * sizeof(payapi_tool_t), MALLOC_CAP_SPIRAM);
        if (!nb) {
            ESP_LOGE(TAG, "tools_append: PSRAM realloc failed");
            return false;
        }
        s_tools    = nb;
        s_tool_cap = new_cap;
    }
    s_tools[s_tool_count++] = *t;
    return true;
}

// Upsert a provider status entry. fqn is strdup'd on insert.
static void status_upsert_locked(const char *fqn, bool loaded,
                                 int tool_count, const char *error) {
    if (!fqn) return;
    // Find existing
    for (size_t i = 0; i < s_status_cnt; i++) {
        if (s_status[i].fqn && strcmp(s_status[i].fqn, fqn) == 0) {
            s_status[i].loaded     = loaded;
            s_status[i].tool_count = tool_count;
            s_status[i].tried_at   = (int64_t)time(NULL);
            if (error && *error) {
                snprintf(s_status[i].last_error, sizeof(s_status[i].last_error),
                         "%s", error);
            } else {
                s_status[i].last_error[0] = '\0';
            }
            return;
        }
    }
    // Insert
    if (s_status_cnt >= s_status_cap) {
        size_t new_cap = s_status_cap ? s_status_cap * 2 : 8;
        payapi_provider_status_t *nb = (payapi_provider_status_t *)heap_caps_realloc(
            s_status, new_cap * sizeof(payapi_provider_status_t), MALLOC_CAP_SPIRAM);
        if (!nb) {
            ESP_LOGE(TAG, "status_upsert: PSRAM realloc failed");
            return;
        }
        s_status     = nb;
        s_status_cap = new_cap;
    }
    payapi_provider_status_t *e = &s_status[s_status_cnt++];
    memset(e, 0, sizeof(*e));
    e->fqn        = strdup(fqn);
    e->loaded     = loaded;
    e->tool_count = tool_count;
    e->tried_at   = (int64_t)time(NULL);
    if (error && *error) {
        snprintf(e->last_error, sizeof(e->last_error), "%s", error);
    }
}

// ---------------------------------------------------------------------------
// FNV-1a hash (32-bit)
// ---------------------------------------------------------------------------

static uint32_t fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
    return h;
}

// ---------------------------------------------------------------------------
// Tool name builder
// ---------------------------------------------------------------------------
// out must be PAYAPI_TOOL_NAME_LEN bytes.
// Format: pay_<short_fqn:≤12>_<short_op:≤12>_<4hex>
// short_fqn/short_op: lowercase alphanumeric only; op is last path segment.

static void build_tool_name(char out[PAYAPI_TOOL_NAME_LEN],
                             const char *fqn, const char *method,
                             const char *path) {
    // short_fqn: keep lowercase alnum, max 12 chars
    char short_fqn[13] = {0};
    size_t fi = 0;
    for (const char *p = fqn; *p && fi < 12; p++) {
        if (isalnum((unsigned char)*p)) {
            short_fqn[fi++] = (char)tolower((unsigned char)*p);
        }
    }
    if (fi == 0) { short_fqn[0] = 'x'; fi = 1; }
    short_fqn[fi] = '\0';

    // short_op: last segment of path (after final '/'), max 12 alnum chars
    const char *last_slash = strrchr(path, '/');
    const char *seg = last_slash ? last_slash + 1 : path;
    char short_op[13] = {0};
    size_t oi = 0;
    for (const char *p = seg; *p && oi < 12; p++) {
        if (isalnum((unsigned char)*p)) {
            short_op[oi++] = (char)tolower((unsigned char)*p);
        }
    }
    if (oi == 0) { short_op[0] = 'r'; oi = 1; }  // root path
    short_op[oi] = '\0';

    // Hash: FNV-1a over "fqn|method|path"
    char hashbuf[256];
    snprintf(hashbuf, sizeof(hashbuf), "%s|%s|%s", fqn, method, path);
    uint32_t h = fnv1a(hashbuf);
    uint16_t h16 = (uint16_t)(h & 0xFFFFu);

    snprintf(out, PAYAPI_TOOL_NAME_LEN, "pay_%s_%s_%04x",
             short_fqn, short_op, (unsigned)h16);
}

// ---------------------------------------------------------------------------
// OpenAPI slimmer
// ---------------------------------------------------------------------------
// Returns cJSON_PrintUnformatted string (caller must free), or NULL on failure.
// Walks one operation object, merges path/query params + request body schema
// into {type:"object", properties:{...}, required:[...]}.

static char *slim_op_to_params_json(const cJSON *operation,
                                    const cJSON *components) {
    cJSON *props    = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    if (!props || !required) {
        cJSON_Delete(props);
        cJSON_Delete(required);
        return NULL;
    }

    // 1. path/query parameters
    cJSON *params = cJSON_GetObjectItem(operation, "parameters");
    if (cJSON_IsArray(params)) {
        cJSON *param;
        cJSON_ArrayForEach(param, params) {
            cJSON *name_j   = cJSON_GetObjectItem(param, "name");
            cJSON *schema_j = cJSON_GetObjectItem(param, "schema");
            if (!cJSON_IsString(name_j) || !name_j->valuestring ||
                !cJSON_IsObject(schema_j)) continue;
            cJSON *clone = cJSON_Duplicate(schema_j, 1);
            if (clone) cJSON_AddItemToObject(props, name_j->valuestring, clone);
            // check required flag on the parameter
            cJSON *req_j = cJSON_GetObjectItem(param, "required");
            if (cJSON_IsTrue(req_j)) {
                cJSON_AddItemToArray(required, cJSON_CreateString(name_j->valuestring));
            }
        }
    }

    // 2. requestBody.content["application/json"].schema
    cJSON *rb = cJSON_GetObjectItem(operation, "requestBody");
    if (cJSON_IsObject(rb)) {
        cJSON *content = cJSON_GetObjectItem(rb, "content");
        if (cJSON_IsObject(content)) {
            cJSON *app_json = cJSON_GetObjectItem(content, "application/json");
            if (cJSON_IsObject(app_json)) {
                cJSON *schema = cJSON_GetObjectItem(app_json, "schema");
                if (cJSON_IsObject(schema)) {
                    // Resolve one $ref hop into components/schemas/<name>
                    cJSON *ref = cJSON_GetObjectItem(schema, "$ref");
                    if (cJSON_IsString(ref) && ref->valuestring &&
                        components) {
                        // Expected form: "#/components/schemas/<name>"
                        const char *refstr = ref->valuestring;
                        const char *prefix = "#/components/schemas/";
                        if (strncmp(refstr, prefix, strlen(prefix)) == 0) {
                            const char *sname = refstr + strlen(prefix);
                            cJSON *schemas = cJSON_GetObjectItem(components, "schemas");
                            if (cJSON_IsObject(schemas)) {
                                cJSON *resolved = cJSON_GetObjectItem(schemas, sname);
                                if (cJSON_IsObject(resolved)) schema = resolved;
                            }
                        }
                    }

                    // NOTE: only one $ref hop is resolved above; any $refs
                    // inside the resolved schema are cloned verbatim — the LLM
                    // will see underspecified properties for nested references.
                    // Merge schema properties into props
                    cJSON *body_props = cJSON_GetObjectItem(schema, "properties");
                    if (cJSON_IsObject(body_props)) {
                        cJSON *prop;
                        cJSON_ArrayForEach(prop, body_props) {
                            if (!prop->string) continue;
                            cJSON *clone = cJSON_Duplicate(prop, 1);
                            if (clone) cJSON_AddItemToObject(props, prop->string, clone);
                        }
                    }
                    // Merge required array
                    cJSON *body_req = cJSON_GetObjectItem(schema, "required");
                    if (cJSON_IsArray(body_req)) {
                        cJSON *r;
                        cJSON_ArrayForEach(r, body_req) {
                            if (cJSON_IsString(r) && r->valuestring)
                                cJSON_AddItemToArray(required, cJSON_CreateString(r->valuestring));
                        }
                    }
                }
            }
        }
    }

    // 3. Wrap result
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(props);
        cJSON_Delete(required);
        return NULL;
    }
    cJSON_AddStringToObject(root, "type", "object");
    cJSON_AddItemToObject(root, "properties", props);
    if (cJSON_GetArraySize(required) > 0) {
        cJSON_AddItemToObject(root, "required", required);
    } else {
        cJSON_Delete(required);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// ---------------------------------------------------------------------------
// Description builder
// ---------------------------------------------------------------------------
// Returns heap_caps_malloc (PSRAM) string. Caller frees.

static char *build_tool_description(const char *title, const char *summary,
                                    const char *method, const char *path,
                                    bool has_free_tier,
                                    uint32_t min_price_usd_cents) {
    char price_hint[32];
    if (has_free_tier && min_price_usd_cents == 0) {
        snprintf(price_hint, sizeof(price_hint), "free");
    } else {
        snprintf(price_hint, sizeof(price_hint), "~$%.3f per call",
                 min_price_usd_cents / 100.0);
    }

    // "<title>: <summary>. <price hint>. <METHOD> <path>"
    char *desc = NULL;
    size_t len = strlen(title ? title : "") + strlen(summary ? summary : "")
               + strlen(price_hint) + strlen(method ? method : "")
               + strlen(path ? path : "") + 32;
    desc = (char *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!desc) return NULL;
    snprintf(desc, len, "%s: %s. %s. %s %s",
             title    ? title    : "",
             summary  ? summary  : "",
             price_hint,
             method   ? method   : "",
             path     ? path     : "");
    return desc;
}

// ---------------------------------------------------------------------------
// Enabled-service check helper
// ---------------------------------------------------------------------------

// Check if a given (fqn, method, path) is enabled in the items array from
// devcfg_pay_enabled_services(). items is the "items" cJSON array.
static bool endpoint_enabled(const cJSON *items, const char *fqn,
                              const char *method, const char *path) {
    if (!cJSON_IsArray(items)) return false;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_fqn = cJSON_GetObjectItem(item, "fqn");
        if (!cJSON_IsString(item_fqn) || !item_fqn->valuestring) continue;
        if (strcmp(item_fqn->valuestring, fqn) != 0) continue;
        // fqn matches
        cJSON *eps = cJSON_GetObjectItem(item, "endpoints");
        if (cJSON_IsString(eps) && eps->valuestring &&
            strcmp(eps->valuestring, "all") == 0) {
            return true;
        }
        if (cJSON_IsArray(eps)) {
            // Build "<METHOD> <path>" string to match against
            char needle[256];
            snprintf(needle, sizeof(needle), "%s %s", method, path);
            cJSON *ep;
            cJSON_ArrayForEach(ep, eps) {
                if (cJSON_IsString(ep) && ep->valuestring &&
                    strcmp(ep->valuestring, needle) == 0) {
                    return true;
                }
            }
        }
        return false;  // fqn found but no matching endpoint
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-provider driver
// ---------------------------------------------------------------------------

bool payapi_refresh_provider(const char *fqn) {
    if (!fqn || !fqn[0]) {
        ESP_LOGW(TAG, "payapi_refresh_provider: null/empty fqn");
        return false;
    }
    ESP_LOGI(TAG, "payapi_refresh_provider: %s", fqn);

    // 1. Snapshot catalog entry under its mutex.
    char  *local_fqn         = NULL;
    char  *local_title       = NULL;
    char  *local_service_url = NULL;
    uint32_t local_min_cents = 0;
    uint32_t local_max_cents = 0;
    bool     local_free_tier = false;

    if (xSemaphoreTake(s_catalog_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_catalog.count; i++) {
            if (s_catalog.items[i].fqn &&
                strcmp(s_catalog.items[i].fqn, fqn) == 0) {
                local_fqn         = strdup(s_catalog.items[i].fqn);
                local_title       = strdup(s_catalog.items[i].title
                                         ? s_catalog.items[i].title : "");
                local_service_url = strdup(s_catalog.items[i].service_url);
                local_min_cents   = s_catalog.items[i].min_price_usd_cents;
                local_max_cents   = s_catalog.items[i].max_price_usd_cents;
                local_free_tier   = s_catalog.items[i].has_free_tier;
                break;
            }
        }
        xSemaphoreGive(s_catalog_mutex);
    }

    if (!local_service_url) {
        // Fall back to the persisted enabled-services blob. The spec guarantees
        // service_url is mirrored there at save time so boot doesn't require a
        // fresh catalog fetch for already-saved providers.
        bool found_via_persisted = false;
        cJSON *persisted = cJSON_Parse(devcfg_pay_enabled_services());
        if (persisted) {
            cJSON *p_items = cJSON_GetObjectItem(persisted, "items");
            cJSON *it;
            cJSON_ArrayForEach(it, p_items) {
                cJSON *e_fqn = cJSON_GetObjectItem(it, "fqn");
                cJSON *e_svc = cJSON_GetObjectItem(it, "service_url");
                if (cJSON_IsString(e_fqn) && cJSON_IsString(e_svc) &&
                    strcmp(e_fqn->valuestring, fqn) == 0) {
                    local_service_url  = strdup(e_svc->valuestring);
                    local_fqn         = strdup(fqn);
                    local_title       = strdup(fqn);   // best available without catalog
                    local_min_cents   = 0;
                    local_max_cents   = 0;
                    local_free_tier   = false;
                    found_via_persisted = true;
                    break;
                }
            }
            cJSON_Delete(persisted);
        }
        if (!found_via_persisted) {
            ESP_LOGW(TAG, "payapi_refresh_provider: %s not in catalog and not in saved enabled-services", fqn);
            free(local_fqn);
            free(local_title);
            free(local_service_url);
            return false;
        }
        ESP_LOGI(TAG, "payapi_refresh_provider: %s using persisted service_url", fqn);
    }

    // 2. Fetch openapi.json. (No mutex held here.)
    char openapi_url[512];
    snprintf(openapi_url, sizeof(openapi_url), "%s/openapi.json", local_service_url);

    char *body = http_get_psram(openapi_url, PAYAPI_OPENAPI_CAP);
    if (!body) {
        ESP_LOGW(TAG, "payapi_refresh_provider: fetch failed for %s", fqn);
        if (s_tools_mutex && xSemaphoreTake(s_tools_mutex, portMAX_DELAY) == pdTRUE) {
            status_upsert_locked(fqn, false, 0, "fetch_failed");
            xSemaphoreGive(s_tools_mutex);
        }
        free(local_fqn); free(local_title); free(local_service_url);
        return false;
    }

    // 3. Parse JSON.
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "payapi_refresh_provider: JSON parse failed for %s", fqn);
        if (s_tools_mutex && xSemaphoreTake(s_tools_mutex, portMAX_DELAY) == pdTRUE) {
            status_upsert_locked(fqn, false, 0, "parse_failed");
            xSemaphoreGive(s_tools_mutex);
        }
        free(local_fqn); free(local_title); free(local_service_url);
        return false;
    }

    // 4. Extract paths + components.
    cJSON *paths      = cJSON_GetObjectItem(root, "paths");
    cJSON *components = cJSON_GetObjectItem(root, "components");  // may be NULL

    // 5. Read enabled-services filter.
    cJSON *enabled_cfg = cJSON_Parse(devcfg_pay_enabled_services());
    cJSON *items_arr   = NULL;
    if (enabled_cfg) {
        items_arr = cJSON_GetObjectItem(enabled_cfg, "items");
    }

    // HTTP method names to walk.
    static const char *METHODS[] = {"get","post","put","patch","delete"};
    static const int   NMETHOD   = 5;

    // 6+7. Under s_tools_mutex: drop old, append new.
    if (!s_tools_mutex) {
        ESP_LOGE(TAG, "tools_mutex not initialized — call payapi_init first");
        cJSON_Delete(enabled_cfg);
        cJSON_Delete(root);
        free(local_fqn); free(local_title); free(local_service_url);
        return false;
    }

    int  added = 0;
    bool oom   = false;
    if (xSemaphoreTake(s_tools_mutex, portMAX_DELAY) == pdTRUE) {
        tools_drop_provider_locked(fqn);

        if (cJSON_IsObject(paths)) {
            cJSON *path_item;
            cJSON_ArrayForEach(path_item, paths) {
                if (oom) break;
                const char *path_str = path_item->string;
                if (!path_str) continue;

                for (int mi = 0; mi < NMETHOD; mi++) {
                    cJSON *op = cJSON_GetObjectItem(path_item, METHODS[mi]);
                    if (!cJSON_IsObject(op)) continue;

                    // Upper-case method for matching & storage
                    char method_up[8];
                    snprintf(method_up, sizeof(method_up), "%s", METHODS[mi]);
                    for (char *c = method_up; *c; c++) *c = (char)toupper((unsigned char)*c);

                    // Check if this endpoint is enabled
                    if (!endpoint_enabled(items_arr, fqn, method_up, path_str)) continue;

                    // Extract summary for description
                    cJSON *sum_j = cJSON_GetObjectItem(op, "summary");
                    const char *summary = (cJSON_IsString(sum_j) && sum_j->valuestring)
                                         ? sum_j->valuestring : "";

                    // Build tool
                    payapi_tool_t t = {0};
                    build_tool_name(t.name, fqn, method_up, path_str);
                    t.fqn               = strdup(fqn);
                    t.service_url       = strdup(local_service_url);
                    t.method            = strdup(method_up);
                    t.path              = strdup(path_str);
                    t.description       = build_tool_description(
                                              local_title, summary,
                                              method_up, path_str,
                                              local_free_tier, local_min_cents);
                    t.params_schema_json = slim_op_to_params_json(op, components);
                    t.price_usd_max_cents = local_max_cents;

                    // If any strdup failed, skip this tool cleanly
                    if (!t.fqn || !t.service_url || !t.method || !t.path) {
                        tool_free_strings(&t);
                        continue;
                    }
                    if (!t.params_schema_json) {
                        // Provide an empty schema rather than failing
                        t.params_schema_json = strdup("{\"type\":\"object\",\"properties\":{}}");
                    }

                    if (tools_append_locked(&t)) {
                        added++;
                    } else {
                        ESP_LOGE(TAG, "%s: tool dropped (registry full / OOM)", local_fqn);
                        tool_free_strings(&t);
                        oom = true;
                        break;
                    }
                }
            }
        }

        // 8. Update status — report OOM loudly so /api/services shows red.
        if (oom) {
            status_upsert_locked(local_fqn, false, added, "registry_oom");
        } else {
            status_upsert_locked(local_fqn, added > 0, added, NULL);
        }
        xSemaphoreGive(s_tools_mutex);
    }

    cJSON_Delete(enabled_cfg);
    cJSON_Delete(root);
    free(local_fqn);
    free(local_title);
    free(local_service_url);

    ESP_LOGI(TAG, "%s: registered %d tools", fqn, added);
    return added > 0;
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------

static void payapi_task(void *arg) {
    (void)arg;
    if (!s_tools_mutex) {
        ESP_LOGE(TAG, "tools_mutex not initialized — call payapi_init first");
        vTaskDelete(NULL);
        return;
    }
    payapi_refresh_catalog();

    cJSON *enabled = cJSON_Parse(devcfg_pay_enabled_services());
    if (enabled) {
        cJSON *items = cJSON_GetObjectItem(enabled, "items");
        if (cJSON_IsArray(items)) {
            cJSON *it;
            cJSON_ArrayForEach(it, items) {
                cJSON *fqn_j = cJSON_GetObjectItem(it, "fqn");
                if (cJSON_IsString(fqn_j) && fqn_j->valuestring &&
                    fqn_j->valuestring[0])
                    payapi_refresh_provider(fqn_j->valuestring);
            }
        }
        cJSON_Delete(enabled);
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API — init
// ---------------------------------------------------------------------------

void payapi_init(void) {
    if (s_initialized) return;

    if (!s_catalog_mutex) {
        s_catalog_mutex = xSemaphoreCreateMutex();
        if (!s_catalog_mutex) {
            ESP_LOGE(TAG, "payapi_init: mutex create failed, aborting");
            return;     // s_initialized stays false → next call retries
        }
    }
    if (!s_tools_mutex) {
        s_tools_mutex = xSemaphoreCreateMutex();
        if (!s_tools_mutex) {
            ESP_LOGE(TAG, "payapi_init: tools mutex create failed, aborting");
            return;
        }
    }
    s_initialized = true;

    // Stack lives in PSRAM (TCB stays in internal RAM), mirroring speech_task.
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        payapi_task, "payapi", 16384, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "payapi_task spawn failed");
    }
}

// ---------------------------------------------------------------------------
// Stubs (Tasks 6-17)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// payapi_attach_tools — append registered tools to a cJSON tools array
// ---------------------------------------------------------------------------
// Called from ai.c::attach_tools() to expose pay.sh tools to the LLM.
// out_array must be a cJSON array object. Skips gracefully if NULL.

int payapi_attach_tools(struct cJSON *out_array)
{
    if (!out_array) return 0;
    if (!s_tools_mutex) return 0;

    if (xSemaphoreTake(s_tools_mutex, portMAX_DELAY) != pdTRUE) return 0;

    int n = 0;
    for (size_t i = 0; i < s_tool_count; i++) {
        payapi_tool_t *t = &s_tools[i];

        cJSON *tool = cJSON_CreateObject();
        if (!tool) continue;

        cJSON_AddStringToObject(tool, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(tool, "function");
        if (!fn) { cJSON_Delete(tool); continue; }

        cJSON_AddStringToObject(fn, "name", t->name);
        cJSON_AddStringToObject(fn, "description",
                                t->description ? t->description : "");

        // Parse the serialized params schema. Fall back to empty object on
        // parse failure so the tool is still visible to the LLM.
        cJSON *params = NULL;
        if (t->params_schema_json && t->params_schema_json[0]) {
            params = cJSON_Parse(t->params_schema_json);
        }
        if (!params) {
            params = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(fn, "parameters", params);

        cJSON_AddItemToArray(out_array, tool);
        n++;
    }

    xSemaphoreGive(s_tools_mutex);
    return n;
}

// ---------------------------------------------------------------------------
// payapi_resolve — map a tool name back to its registry entry
// ---------------------------------------------------------------------------
// Values are strlcpy'd into out under the mutex; out fields are caller-owned
// and safe to use after the call even if the registry is concurrently mutated.

bool payapi_resolve(const char *tool_name, payapi_tool_info_t *out)
{
    if (!tool_name || !out || !s_tools_mutex) return false;

    bool found = false;

    if (xSemaphoreTake(s_tools_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_tool_count; i++) {
            if (strcmp(s_tools[i].name, tool_name) == 0) {
                strlcpy(out->service_url, s_tools[i].service_url ? s_tools[i].service_url : "", sizeof out->service_url);
                strlcpy(out->method,      s_tools[i].method      ? s_tools[i].method      : "", sizeof out->method);
                strlcpy(out->path,        s_tools[i].path        ? s_tools[i].path        : "", sizeof out->path);
                strlcpy(out->fqn,         s_tools[i].fqn         ? s_tools[i].fqn         : "", sizeof out->fqn);
                out->price_usd_max_cents = s_tools[i].price_usd_max_cents;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_tools_mutex);
    }

    return found;
}

x402_guard_decision_t payapi_guard(uint64_t actual_micros,
                                   const char *description,
                                   void *user)
{
    (void)user;

    uint64_t today  = devcfg_spend_today_micros();
    uint64_t auto_  = (uint64_t)devcfg_spend_auto_max_cents()    * 10000ULL;
    uint64_t conf_  = (uint64_t)devcfg_spend_confirm_max_cents() * 10000ULL;
    uint64_t daily_ = (uint64_t)devcfg_spend_daily_cap_cents()   * 10000ULL;

    if (today + actual_micros > daily_) {
        ESP_LOGW(TAG, "guard REFUSE: would exceed daily cap "
                       "(today=%llu + actual=%llu > daily=%llu)",
                 today, actual_micros, daily_);
        return X402_GUARD_REFUSE;
    }
    if (actual_micros <= auto_) {
        return X402_GUARD_AUTO;
    }
    if (actual_micros > conf_) {
        ESP_LOGW(TAG, "guard REFUSE: actual=%llu > confirm cap=%llu",
                 actual_micros, conf_);
        return X402_GUARD_REFUSE;
    }

    // Hold-confirm window. Speak the price first IF >= $1.00.
    if (actual_micros >= 1000000ULL) {
        char say[96];
        double d = (double)actual_micros / 1e6;
        snprintf(say, sizeof(say), "%.2f to confirm. Hold the screen.", d);
        voice_speak_chunk(say);  // non-blocking: copies into queue internally
    }

    return pay_confirm_screen_show_blocking(actual_micros,
                                            description ? description : "",
                                            s_ptt_gen);
}

// ---------------------------------------------------------------------------
// payapi_status_json — per-provider load status for /api/services
// ---------------------------------------------------------------------------
// Returns a cJSON object: {providers:[{fqn,loaded,tool_count,error?,tried_at}],
// catalog_synced_at:<unix>}. Acquires s_tools_mutex then s_catalog_mutex
// separately — never both at once.

struct cJSON *payapi_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *providers = cJSON_CreateArray();
    if (!providers) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToObject(root, "providers", providers);

    // --- s_tools_mutex: read s_status[] ---
    if (s_tools_mutex && xSemaphoreTake(s_tools_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < s_status_cnt; i++) {
            payapi_provider_status_t *st = &s_status[i];
            if (!st->fqn) continue;

            cJSON *entry = cJSON_CreateObject();
            if (!entry) continue;

            cJSON_AddStringToObject(entry, "fqn",        st->fqn);
            cJSON_AddBoolToObject(  entry, "loaded",     st->loaded);
            cJSON_AddNumberToObject(entry, "tool_count", st->tool_count);
            if (st->last_error[0] != '\0') {
                cJSON_AddStringToObject(entry, "error", st->last_error);
            }
            cJSON_AddNumberToObject(entry, "tried_at", (double)st->tried_at);

            cJSON_AddItemToArray(providers, entry);
        }
        xSemaphoreGive(s_tools_mutex);
    }

    // --- s_catalog_mutex: read synced_at only ---
    int64_t synced_at = 0;
    if (s_catalog_mutex && xSemaphoreTake(s_catalog_mutex, portMAX_DELAY) == pdTRUE) {
        synced_at = s_catalog.synced_at;
        xSemaphoreGive(s_catalog_mutex);
    }

    cJSON_AddNumberToObject(root, "catalog_synced_at", (double)synced_at);
    return root;
}

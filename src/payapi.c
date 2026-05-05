// ---------------------------------------------------------------------------
// payapi.c — pay.sh dispatcher
// ---------------------------------------------------------------------------
#include "payapi.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_sta.h"

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
// Module state
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_catalog_mutex = NULL;
static payapi_catalog_t  s_catalog       = {0};
static bool              s_initialized   = false;

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

    return buf;
}

// ---------------------------------------------------------------------------
// Catalog parser helpers
// ---------------------------------------------------------------------------

// Convert USD double to cents with rounding, clamped to UINT32_MAX.
static uint32_t usd_to_cents(double v) {
    if (v <= 0.0) return 0;
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
// Background task
// ---------------------------------------------------------------------------

static void payapi_task(void *arg) {
    (void)arg;
    payapi_refresh_catalog();
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API — init
// ---------------------------------------------------------------------------

void payapi_init(void) {
    if (s_initialized) return;
    s_initialized = true;

    if (!s_catalog_mutex) {
        s_catalog_mutex = xSemaphoreCreateMutex();
    }

    // Stack lives in PSRAM (TCB stays in internal RAM), mirroring speech_task.
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        payapi_task, "payapi", 16384, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "payapi_task spawn failed");
    }
}

// ---------------------------------------------------------------------------
// Stubs (Tasks 5-17)
// ---------------------------------------------------------------------------

bool payapi_refresh_provider(const char *fqn)
{
    ESP_LOGI(TAG, "payapi_refresh_provider: %s", fqn ? fqn : "(null)");
    return false;
}

bool payapi_resolve(const char *tool_name, payapi_tool_info_t *out)
{
    ESP_LOGI(TAG, "payapi_resolve: %s", tool_name ? tool_name : "(null)");
    return false;
}

void payapi_attach_tools(struct cJSON *out_array)
{
    ESP_LOGI(TAG, "payapi_attach_tools");
    (void)out_array;
}

x402_guard_decision_t payapi_guard(uint64_t actual_micros,
                                   const char *description,
                                   void *user)
{
    ESP_LOGI(TAG, "payapi_guard");
    (void)actual_micros;
    (void)description;
    (void)user;
    return X402_GUARD_AUTO;
}

struct cJSON *payapi_status_json(void)
{
    ESP_LOGI(TAG, "payapi_status_json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "providers", cJSON_CreateArray());
    cJSON_AddNumberToObject(root, "catalog_synced_at", 0);
    return root;
}

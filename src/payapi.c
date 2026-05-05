// ---------------------------------------------------------------------------
// payapi.c — pay.sh dispatcher (stubs)
// ---------------------------------------------------------------------------
#include "payapi.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "payapi";

void payapi_init(void)
{
    ESP_LOGI(TAG, "payapi_init");
}

bool payapi_refresh_catalog(void)
{
    ESP_LOGI(TAG, "payapi_refresh_catalog");
    return false;
}

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

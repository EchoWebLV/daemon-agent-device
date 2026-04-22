// ---------------------------------------------------------------------------
//  SOL/USD price. See price.h.
//
//  TLS to CoinGecko via esp_http_client + crt_bundle (IDF's baked-in root
//  CA bundle — no per-endpoint PEMs to maintain). JSON parsed with cJSON.
//  Background task serializes fetches and sleeps on a binary semaphore;
//  that way callers never block and we can batch triggers without piling
//  up concurrent TLS handshakes.
// ---------------------------------------------------------------------------
#include "price.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifi_sta.h"

static const char *TAG = "price";

// --- Cached state ------------------------------------------------------------
static double              s_price_usd     = 0.0;

// --- Background task ---------------------------------------------------------
static TaskHandle_t        s_task     = NULL;
static SemaphoreHandle_t   s_trigger  = NULL;

// --- Response collector ------------------------------------------------------
// esp_http_client streams the response via events. For small JSON responses
// (CoinGecko is ~40 bytes) we just append into a stack buffer; the handler
// stops writing once it's full, and we parse whatever we got.
#define PRICE_RSP_MAX 512
typedef struct {
    char   buf[PRICE_RSP_MAX];
    size_t len;
} rsp_buf_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rsp_buf_t *r = (rsp_buf_t *)evt->user_data;
    if (!r) return ESP_OK;
    size_t room = sizeof(r->buf) - 1 - r->len;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(r->buf + r->len, evt->data, take);
    r->len += take;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// --- Blocking refresh --------------------------------------------------------
void price_refresh(void) {
    if (!wifi_sta_is_connected()) return;

    rsp_buf_t rsp = { .len = 0 };
    esp_http_client_config_t cfg = {
        .url               = "https://api.coingecko.com/api/v3/"
                             "simple/price?ids=solana&vs_currencies=usd",
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_http_event,
        .user_data         = &rsp,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) { ESP_LOGW(TAG, "client init failed"); return; }

    esp_err_t err  = esp_http_client_perform(http);
    int       code = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform: %s", esp_err_to_name(err));
        return;
    }
    if (code != 200) {
        ESP_LOGW(TAG, "HTTP %d", code);
        return;
    }

    // Body shape: {"solana":{"usd":123.45}}
    cJSON *root = cJSON_Parse(rsp.buf);
    if (!root) { ESP_LOGW(TAG, "json parse"); return; }
    cJSON *solana = cJSON_GetObjectItemCaseSensitive(root, "solana");
    cJSON *usd    = solana ? cJSON_GetObjectItemCaseSensitive(solana, "usd") : NULL;
    if (cJSON_IsNumber(usd) && usd->valuedouble > 0) {
        s_price_usd = usd->valuedouble;
        ESP_LOGI(TAG, "SOL $%.2f", s_price_usd);
    }
    cJSON_Delete(root);
}

// --- Background plumbing -----------------------------------------------------
static void price_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_trigger, portMAX_DELAY) == pdTRUE) {
            price_refresh();
        }
    }
}

static void ensure_task(void) {
    if (s_task) return;
    s_trigger = xSemaphoreCreateBinary();
    // 6 KB covers TLS handshake + cJSON parse for the 40-byte response.
    // Smaller stacks risk tripping the canary under mbedtls handshake.
    xTaskCreatePinnedToCore(price_task, "price", 6144, NULL, 5, &s_task, 1);
}

// --- Public API --------------------------------------------------------------
bool price_begin(void) {
    s_price_usd = 0.0;
    return true;
}

void price_request_refresh(void) {
    ensure_task();
    if (s_trigger) xSemaphoreGive(s_trigger);
}

double price_sol_usd(void) { return s_price_usd; }

void price_display_string(char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (s_price_usd <= 0) {
        snprintf(out, cap, "SOL --");
    } else {
        snprintf(out, cap, "SOL $%.2f", s_price_usd);
    }
}

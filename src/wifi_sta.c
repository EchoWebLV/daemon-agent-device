// ---------------------------------------------------------------------------
//  Wi-Fi STA bring-up. See wifi_sta.h.
//
//  Connection flow uses an EventGroup: IP_EVENT_STA_GOT_IP sets CONNECTED,
//  WIFI_EVENT_STA_DISCONNECTED retries up to MAX_RETRIES then sets FAILED.
//  That lets us expose a simple blocking `connect(..., timeout_ms)` on top
//  of the asynchronous esp_wifi event machine.
// ---------------------------------------------------------------------------
#include "wifi_sta.h"
#include "devcfg.h"
#include "secrets.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

#define MAX_RETRIES       5
#define CONNECTED_BIT     BIT0
#define FAILED_BIT        BIT1

static bool              s_driver_ready = false;
static bool              s_connected    = false;
static esp_netif_t      *s_netif        = NULL;
static EventGroupHandle_t s_events      = NULL;
static int               s_retries      = 0;
static esp_ip4_addr_t    s_ip           = { 0 };
static char              s_ssid[33]     = "";    // current association
static int8_t            s_rssi         = 0;

// --- Event handling ---------------------------------------------------------
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "disconnect; retry %d/%d", s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "connect failed after %d retries", MAX_RETRIES);
            xEventGroupSetBits(s_events, FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_ip        = evt->ip_info.ip;
        s_retries   = 0;
        s_connected = true;
        // Record SSID + RSSI at the moment the IP lands so the settings
        // screen doesn't have to poll the driver every frame.
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strlcpy(s_ssid, (const char *)ap.ssid, sizeof(s_ssid));
            s_rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&s_ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

// --- One-shot driver init ---------------------------------------------------
esp_err_t wifi_sta_init(void) {
    if (s_driver_ready) return ESP_OK;

    ESP_RETURN_ON_ERROR(esp_netif_init(),              TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(),TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif sta");

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_event, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_event, NULL, NULL),
        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode sta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),
                        TAG, "wifi ram storage");

    s_driver_ready = true;
    ESP_LOGI(TAG, "STA driver ready");
    return ESP_OK;
}

// --- Public connect API -----------------------------------------------------
esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           uint32_t timeout_ms) {
    ESP_RETURN_ON_FALSE(s_driver_ready, ESP_ERR_INVALID_STATE,
                        TAG, "wifi_sta_init first");
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG,
                        TAG, "empty ssid");

    // Reset event state + retry counter for this attempt.
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAILED_BIT);
    s_retries   = 0;
    s_connected = false;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid,     ssid,                    sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password ? password : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // accept any auth; Waveshare panel
    wc.sta.pmf_cfg.capable    = true;             // boards we target all support PMF

    // On repeat calls (changing networks at runtime), drop the old assoc
    // before reconfiguring; esp_wifi_set_config won't reconnect on its own.
    esp_wifi_disconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc),
                        TAG, "set_config");
    esp_err_t err = esp_wifi_start();
    // esp_wifi_start returns INVALID_STATE if already started; that's fine
    // (STA_START handler calls esp_wifi_connect, but we also kick it manually
    // below for the repeat-call case).
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_connect();

    ESP_LOGI(TAG, "connecting to '%s'...", ssid);

    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, CONNECTED_BIT | FAILED_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & CONNECTED_BIT) {
        // Persist so future boots pick it up.
        devcfg_set_wifi(ssid, password ? password : "");
        return ESP_OK;
    }
    if (bits & FAILED_BIT) return ESP_FAIL;

    ESP_LOGW(TAG, "connect timeout (%lu ms)", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

// --- Begin: NVS first, secrets.h fallback -----------------------------------
esp_err_t wifi_sta_begin(uint32_t timeout_ms) {
    ESP_RETURN_ON_ERROR(wifi_sta_init(), TAG, "init");

    const char *ssid = devcfg_wifi_ssid();
    const char *pass = devcfg_wifi_password();
    if (ssid[0] == '\0') {
        // No stored creds — use the compile-time fallback.
        ssid = WIFI_SSID;
        pass = WIFI_PASSWORD;
        ESP_LOGI(TAG, "no NVS creds; using secrets.h fallback");
    } else {
        ESP_LOGI(TAG, "using NVS creds for '%s'", ssid);
    }
    return wifi_sta_connect(ssid, pass, timeout_ms);
}

// --- Disconnect / status ----------------------------------------------------
void wifi_sta_drop(void) {
    if (!s_driver_ready) return;
    esp_wifi_disconnect();
    s_connected = false;
    memset(&s_ip, 0, sizeof(s_ip));
}

bool wifi_sta_is_connected(void) { return s_connected; }

void wifi_sta_ip_str(char *out, size_t cap) {
    if (cap == 0) return;
    if (!s_connected) { out[0] = '\0'; return; }
    snprintf(out, cap, IPSTR, IP2STR(&s_ip));
}

const char *wifi_sta_current_ssid(void) { return s_connected ? s_ssid : ""; }
int8_t       wifi_sta_current_rssi(void) { return s_connected ? s_rssi : 0;  }

// ---------------------------------------------------------------------------
// Scan: blocking, de-duplicated, RSSI-sorted.
// ---------------------------------------------------------------------------
size_t wifi_sta_scan(wifi_sta_scan_ap_t *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (!s_driver_ready) {
        ESP_LOGW(TAG, "scan before init");
        return 0;
    }

    // The driver requires the STA interface to be started. wifi_sta_connect
    // calls esp_wifi_start on first attempt; if we've never connected we
    // have to start it ourselves. Ignoring "already started" is fine.
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGW(TAG, "scan: wifi_start: %s", esp_err_to_name(start_err));
        return 0;
    }

    wifi_scan_config_t scan_cfg = {0};   // default: active, all channels, 120 ms/ch
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, /*block=*/true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t n_found = 0;
    esp_wifi_scan_get_ap_num(&n_found);
    if (n_found == 0) {
        ESP_LOGI(TAG, "scan: 0 APs visible");
        return 0;
    }

    // Pull up to 32 raw records from the driver so we have a reasonable
    // dedupe pool even when the caller only asked for a handful.
    const uint16_t RAW_CAP = 32;
    wifi_ap_record_t *raw = calloc(RAW_CAP, sizeof(*raw));
    if (!raw) {
        ESP_LOGW(TAG, "scan: oom (%u records)", (unsigned)n_found);
        esp_wifi_clear_ap_list();
        return 0;
    }
    uint16_t got = (n_found < RAW_CAP) ? n_found : RAW_CAP;
    esp_wifi_scan_get_ap_records(&got, raw);

    // Dedupe: prefer the strongest RSSI for each SSID. Case-sensitive match
    // — some APs advertise the same SSID with different capitalisation and
    // the user really has chosen one or the other.
    size_t n_out = 0;
    for (uint16_t i = 0; i < got && n_out < cap; ++i) {
        const char *ssid = (const char *)raw[i].ssid;
        if (ssid[0] == '\0') continue;  // skip hidden networks

        // Already seen?
        ssize_t dupe = -1;
        for (size_t j = 0; j < n_out; ++j) {
            if (strncmp(out[j].ssid, ssid, sizeof(out[j].ssid)) == 0) { dupe = (ssize_t)j; break; }
        }
        if (dupe >= 0) {
            if (raw[i].rssi > out[dupe].rssi) out[dupe].rssi = raw[i].rssi;
            continue;
        }

        strlcpy(out[n_out].ssid, ssid, sizeof(out[n_out].ssid));
        out[n_out].rssi      = raw[i].rssi;
        out[n_out].auth_open = (raw[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
        n_out++;
    }
    free(raw);
    esp_wifi_clear_ap_list();

    // Sort by RSSI descending (strongest first). n_out is small enough
    // that bubble-sort is fine — no need for qsort's indirection.
    for (size_t i = 0; i + 1 < n_out; ++i) {
        for (size_t j = 0; j + 1 + i < n_out; ++j) {
            if (out[j].rssi < out[j + 1].rssi) {
                wifi_sta_scan_ap_t tmp = out[j];
                out[j] = out[j + 1];
                out[j + 1] = tmp;
            }
        }
    }

    ESP_LOGI(TAG, "scan: %u dedup'd APs (from %u raw / %u total)",
             (unsigned)n_out, (unsigned)got, (unsigned)n_found);
    return n_out;
}

// ---------------------------------------------------------------------------
//  First-boot captive-portal wizard. See wizard.h.
//
//  Boot flow:
//      1. wizard_is_first_run() returns true if NVS doesn't have the
//         "done" flag yet. app_main calls wizard_start() on first run.
//      2. wizard_start() switches Wi-Fi to APSTA mode, brings up an AP
//         "daemon-setup-XXXX", registers /  and /wizard URI handlers
//         on the existing httpd (server.c already runs at boot).
//      3. User connects from their phone, opens daemon.local or the
//         captive portal URL, fills the form, hits submit.
//      4. POST /wizard parses the JSON body, runs wizard_apply_form()
//         which writes Wi-Fi + owner_pubkey + PIN-sealed seed into NVS,
//         marks done, returns 200, and reboots.
//      5. After reboot, wizard_is_first_run() returns false and the
//         daemon comes up in normal STA mode.
// ---------------------------------------------------------------------------
#include "wizard.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "base58.h"
#include "devcfg.h"
#include "pin.h"
#include "secrets.h"
#include "server.h"
#include "wifi_sta.h"

static const char *TAG = "wizard";

// Embedded HTML page from scripts/embed_assets.py.
extern const char  wizard_html_start[];
extern const size_t wizard_html_len;

// NVS namespace for wizard-specific state (separate from devcfg's "cfg").
#define WIZ_NVS_NS         "wizard"
#define WIZ_NVS_KEY_DONE   "done"
#define WIZ_NVS_KEY_OWNER  "owner_pk"

#define MAX_OWNER_PK_LEN   48

static httpd_handle_t s_httpd     = NULL;   // borrowed from server.c
static esp_netif_t   *s_ap_netif  = NULL;
static char           s_owner_pk_cache[MAX_OWNER_PK_LEN] = {0};
static bool           s_owner_pk_loaded = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static bool nvs_read_bool(const char *key, bool *out) {
    nvs_handle_t h;
    if (nvs_open(WIZ_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (out) *out = (v != 0);
    return true;
}

static esp_err_t nvs_write_bool(const char *key, bool v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIZ_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *s) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIZ_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_read_str(const char *key, char *out, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(WIZ_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

// ---------------------------------------------------------------------------
// Public state queries
// ---------------------------------------------------------------------------
bool wizard_is_first_run(void) {
    bool done = false;
    if (!nvs_read_bool(WIZ_NVS_KEY_DONE, &done)) return true;
    return !done;
}

void wizard_mark_done(void) {
    esp_err_t err = nvs_write_bool(WIZ_NVS_KEY_DONE, true);
    if (err != ESP_OK) ESP_LOGW(TAG, "mark_done: %s", esp_err_to_name(err));
}

const char *wizard_owner_pubkey(void) {
    if (s_owner_pk_loaded) {
        return s_owner_pk_cache[0] ? s_owner_pk_cache : NULL;
    }
    if (nvs_read_str(WIZ_NVS_KEY_OWNER, s_owner_pk_cache, sizeof s_owner_pk_cache)) {
        s_owner_pk_loaded = true;
        return s_owner_pk_cache[0] ? s_owner_pk_cache : NULL;
    }
    s_owner_pk_loaded = true;
    s_owner_pk_cache[0] = '\0';
    return NULL;
}

// ---------------------------------------------------------------------------
// Form persistence
// ---------------------------------------------------------------------------
esp_err_t wizard_apply_form(const wizard_form_t *form) {
    if (!form) return ESP_ERR_INVALID_ARG;

    // 1. Wi-Fi credentials → devcfg (already-supported NVS-backed setter)
    if (form->ssid[0]) {
        devcfg_set_wifi(form->ssid, form->password);
    }

    // 2. Owner pubkey: validate it decodes to 32 bytes, then persist.
    if (form->owner_pubkey[0]) {
        uint8_t obuf[32];
        int n = base58_decode(form->owner_pubkey, obuf, sizeof obuf);
        if (n != 32) {
            ESP_LOGW(TAG, "owner_pubkey decode failed (%d)", n);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = nvs_write_str(WIZ_NVS_KEY_OWNER, form->owner_pubkey);
        if (err != ESP_OK) return err;
        s_owner_pk_loaded = false;   // re-read on next access
    }

    // 3. Device seed → sealed under the PIN.
    //    Per-device key generation: each shipping unit needs its own
    //    wallet, so by default we draw 32 fresh bytes from the hardware
    //    RNG. SOLANA_KEY in secrets.h is honored as a *dev-only override*
    //    — useful when you want a known wallet on your bench device.
    //    Anything else (placeholder, missing, or undecodable) falls
    //    through to fresh generation.
    if (form->pin[0]) {
        uint8_t seed[PIN_MAX_SEED_LEN] = {0};
        size_t  seed_len = 0;

        const char *override = SOLANA_KEY;
        if (override && override[0] && strncmp(override, "PASTE-", 6) != 0) {
            int n = base58_decode(override, seed, sizeof seed);
            if (n == 32 || n == 64) {
                seed_len = (size_t)n;
                ESP_LOGI(TAG, "seed: using SOLANA_KEY override from secrets.h");
            }
        }
        if (seed_len == 0) {
            // esp_fill_random is cryptographically secure once Wi-Fi/BT
            // is up (which it is by the time the wizard runs).
            esp_fill_random(seed, 32);
            seed_len = 32;
            ESP_LOGI(TAG, "seed: generated fresh 32 B from hardware RNG");
        }

        pin_status_t st = pin_setup(form->pin, seed, seed_len);
        memset(seed, 0, sizeof seed);
        if (st != PIN_OK) {
            ESP_LOGW(TAG, "pin_setup err=%d", st);
            return ESP_FAIL;
        }
    }

    wizard_mark_done();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static esp_err_t handle_wizard_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, wizard_html_start, wizard_html_len);
}

// Triggers a STA-side scan and returns the deduped, RSSI-sorted result as
// JSON. Cleanly escapes SSIDs (most are plain ASCII but some legitimately
// contain spaces, punctuation, or UTF-8). The radio is in APSTA mode while
// the wizard runs — connected clients may see a brief glitch as the radio
// hops channels during the scan, but the HTTP request itself rides through.
static esp_err_t handle_wizard_scan(httpd_req_t *req) {
    const size_t MAX_APS = 16;
    wifi_sta_scan_ap_t *aps = calloc(MAX_APS, sizeof *aps);
    if (!aps) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t n = wifi_sta_scan(aps, MAX_APS);

    cJSON *root  = cJSON_CreateObject();
    cJSON *arr   = cJSON_AddArrayToObject(root, "networks");
    if (!root || !arr) { cJSON_Delete(root); free(aps); return ESP_FAIL; }
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject  (o, "open", aps[i].auth_open != 0);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t handle_wizard_post(httpd_req_t *req) {
    if (req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_FAIL;
    }
    char buf[1100];
    int n = httpd_req_recv(req, buf, sizeof buf - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    wizard_form_t form = {0};
    const cJSON *j;

#define COPY_STR(field, key) \
    j = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsString(j) && j->valuestring) { \
        strlcpy(form.field, j->valuestring, sizeof form.field); \
    }

    COPY_STR(ssid,         "ssid");
    COPY_STR(password,     "password");
    COPY_STR(owner_pubkey, "owner_pubkey");
    COPY_STR(pin,          "pin");
#undef COPY_STR
    cJSON_Delete(root);

    if (!form.ssid[0] || !form.owner_pubkey[0] || !form.pin[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"missing field\"}");
        return ESP_FAIL;
    }

    esp_err_t err = wizard_apply_form(&form);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char body[96];
        snprintf(body, sizeof body, "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, body);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // Reboot after a short delay so the response actually flushes.
    ESP_LOGI(TAG, "wizard done — restarting in 3 s");
    static esp_timer_handle_t restart_timer;
    static const esp_timer_create_args_t args = {
        .callback        = (void (*)(void *))esp_restart,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "wiz_reboot",
    };
    if (!restart_timer) esp_timer_create(&args, &restart_timer);
    esp_timer_start_once(restart_timer, 3000000);
    return ESP_OK;
}

static const httpd_uri_t s_uri_get = {
    .uri      = "/wizard",
    .method   = HTTP_GET,
    .handler  = handle_wizard_get,
    .user_ctx = NULL,
};
static const httpd_uri_t s_uri_post = {
    .uri      = "/wizard",
    .method   = HTTP_POST,
    .handler  = handle_wizard_post,
    .user_ctx = NULL,
};
static const httpd_uri_t s_uri_scan = {
    .uri      = "/wizard/scan",
    .method   = HTTP_GET,
    .handler  = handle_wizard_scan,
    .user_ctx = NULL,
};

// ---------------------------------------------------------------------------
// Wi-Fi AP bring-up
// ---------------------------------------------------------------------------
bool wizard_compute_ap_ssid(char *out, size_t cap) {
    if (!out || cap < 18) return false;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) return false;
    snprintf(out, cap, "daemon-setup-%02X%02X", mac[4], mac[5]);
    return true;
}

static esp_err_t bring_up_ap(void) {
    if (s_ap_netif) return ESP_OK;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "create AP netif failed");
        return ESP_FAIL;
    }

    char ssid[33];
    if (!wizard_compute_ap_ssid(ssid, sizeof ssid)) return ESP_FAIL;

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg.required = false,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof ap_cfg.ap.ssid);
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(),                          TAG, "wifi start");
    ESP_LOGI(TAG, "wizard AP up: %s (open)", ssid);
    return ESP_OK;
}

esp_err_t wizard_start(void) {
    if (s_httpd) return ESP_OK;

    ESP_RETURN_ON_ERROR(bring_up_ap(), TAG, "bring_up_ap");

    // Reuse server.c's running httpd instead of fighting for port 80.
    s_httpd = (httpd_handle_t)server_http_handle();
    if (!s_httpd) {
        ESP_LOGE(TAG, "server.c httpd not running yet — call server_start first");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &s_uri_get),  TAG, "register GET");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &s_uri_post), TAG, "register POST");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &s_uri_scan), TAG, "register scan");
    ESP_LOGI(TAG, "wizard started; visit http://192.168.4.1/wizard");
    return ESP_OK;
}

esp_err_t wizard_stop(void) {
    if (s_httpd) {
        httpd_unregister_uri_handler(s_httpd, "/wizard", HTTP_GET);
        httpd_unregister_uri_handler(s_httpd, "/wizard", HTTP_POST);
        httpd_unregister_uri_handler(s_httpd, "/wizard/scan", HTTP_GET);
        s_httpd = NULL;   // borrowed handle, don't stop it
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    return ESP_OK;
}

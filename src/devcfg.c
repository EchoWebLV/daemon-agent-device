// ---------------------------------------------------------------------------
//  NVS-backed device settings + LCD backlight PWM. See devcfg.h.
// ---------------------------------------------------------------------------
#include "devcfg.h"
#include "board.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "devcfg";

// ---- Backlight config -------------------------------------------------------
// Pin matches src/board.h (GPIO47 on the BOX-3B). Owned exclusively by
// devcfg via LEDC PWM; no other subsystem touches the line.
#define BL_PIN              BOARD_LCD_PIN_BL
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ     5000
#define BL_LEDC_RESOLUTION  LEDC_TIMER_8_BIT   // 0..255
#define BL_MIN_DUTY         12                  // never fully dark

// ---- NVS config -------------------------------------------------------------
#define NVS_NAMESPACE       "daemon"
#define KEY_VOLUME          "vol"
#define KEY_BRIGHTNESS      "bri"
#define KEY_BLUETOOTH       "bt"
#define KEY_WIFI_SSID       "wf_ssid"
#define KEY_WIFI_PASSWORD   "wf_pass"
#define KEY_LLM_MODEL       "llm_model"
#define KEY_VOICE_ID        "voice_id"
#define KEY_PERSONALITY     "persona"
#define KEY_SVC_CUSTOM      "svc_custom"
#define KEY_SVC_ENABLED     "svc_enabled"

// Upper bounds on strings we persist. WPA2 passwords cap at 63 chars + NUL;
// SSIDs at 32 + NUL. We round up for headroom (hidden SSIDs / WPA3).
#define SSID_MAX            64
#define PASSWORD_MAX        96
// Model ids are short ("provider/model" shape caps out around 64 chars);
// personality is free-form user-written text that shows up in the AI
// system prompt — bound it to 1 KB so we can't blow past the chat body cap.
#define LLM_MODEL_MAX       96
// ElevenLabs voice ids are 20-char alphanumeric tokens; 32 + NUL is plenty.
#define VOICE_ID_MAX        48
#define PERSONALITY_MAX     1024
// customServices is the full JSON array of user-added services; enabled
// is a JSON array of ID strings. Sized so typical usage (~8 services) fits
// comfortably; the HTTP /config POST cap in server.c must match or exceed
// the custom-services bound or the library saves will silently truncate.
#define SVC_CUSTOM_MAX      4096
#define SVC_ENABLED_MAX      512

// ---- Module state -----------------------------------------------------------
static bool    s_ready       = false;
static uint8_t s_volume      = 21;
static uint8_t s_brightness  = 255;
static bool    s_bluetooth   = false;
static char    s_ssid[SSID_MAX];
static char    s_password[PASSWORD_MAX];
static char    s_llm_model[LLM_MODEL_MAX];
static char    s_voice_id[VOICE_ID_MAX];
static char    s_personality[PERSONALITY_MAX];
static char    s_svc_custom[SVC_CUSTOM_MAX];
static char    s_svc_enabled[SVC_ENABLED_MAX];

// Small helpers ---------------------------------------------------------------
static void apply_brightness(uint8_t b) {
    if (b < BL_MIN_DUTY) b = BL_MIN_DUTY;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, b);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

static esp_err_t ledc_init(void) {
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RESOLUTION,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num   = BL_PIN,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 255,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc channel");
    return ESP_OK;
}

// Open NVS handle with the desired mode. Returns ESP_OK even when the
// namespace doesn't exist yet on read (caller gets the defaults it passed).
static esp_err_t nvs_open_ns(nvs_open_mode_t mode, nvs_handle_t *out) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, out);
    if (err == ESP_ERR_NVS_NOT_FOUND && mode == NVS_READONLY) {
        *out = 0;
        return ESP_OK;  // no writes yet, so every read returns default
    }
    return err;
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *out, uint8_t dflt) {
    if (h == 0) { *out = dflt; return; }
    if (nvs_get_u8(h, key, out) != ESP_OK) *out = dflt;
}

static void load_bool(nvs_handle_t h, const char *key, bool *out, bool dflt) {
    if (h == 0) { *out = dflt; return; }
    uint8_t v = 0;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *out = (v != 0);
    else                                   *out = dflt;
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    if (h == 0) return;
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

static void save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_str(const char *key, const char *v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (v == NULL || v[0] == '\0') nvs_erase_key(h, key);
    else                            nvs_set_str(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

// Public API ------------------------------------------------------------------
esp_err_t devcfg_init(void) {
    if (s_ready) return ESP_OK;

    ESP_RETURN_ON_ERROR(ledc_init(), TAG, "ledc init");

    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open_ns(NVS_READONLY, &h), TAG, "nvs open");

    load_u8  (h, KEY_VOLUME,        &s_volume,     21);
    load_u8  (h, KEY_BRIGHTNESS,    &s_brightness, 255);
    load_bool(h, KEY_BLUETOOTH,     &s_bluetooth,  false);
    load_str (h, KEY_WIFI_SSID,     s_ssid,        sizeof(s_ssid));
    load_str (h, KEY_WIFI_PASSWORD, s_password,    sizeof(s_password));
    load_str (h, KEY_LLM_MODEL,     s_llm_model,   sizeof(s_llm_model));
    load_str (h, KEY_VOICE_ID,      s_voice_id,    sizeof(s_voice_id));
    load_str (h, KEY_PERSONALITY,   s_personality, sizeof(s_personality));
    load_str (h, KEY_SVC_CUSTOM,    s_svc_custom,  sizeof(s_svc_custom));
    load_str (h, KEY_SVC_ENABLED,   s_svc_enabled, sizeof(s_svc_enabled));

    if (h != 0) nvs_close(h);

    apply_brightness(s_brightness);
    s_ready = true;

    ESP_LOGI(TAG, "loaded: vol=%u bri=%u bt=%d ssid='%s'",
             s_volume, s_brightness, s_bluetooth ? 1 : 0,
             s_ssid[0] ? s_ssid : "(none)");
    return ESP_OK;
}

uint8_t devcfg_volume(void)     { return s_volume; }
uint8_t devcfg_brightness(void) { return s_brightness; }
bool    devcfg_bluetooth(void)  { return s_bluetooth; }

void devcfg_set_volume(uint8_t v) {
    if (v > 21) v = 21;
    if (v == s_volume) return;
    s_volume = v;
    save_u8(KEY_VOLUME, v);
}

void devcfg_set_brightness(uint8_t b) {
    if (b == s_brightness) return;
    s_brightness = b;
    apply_brightness(b);
    save_u8(KEY_BRIGHTNESS, b);
}

void devcfg_set_bluetooth(bool on) {
    if (on == s_bluetooth) return;
    s_bluetooth = on;
    save_u8(KEY_BLUETOOTH, on ? 1 : 0);
}

const char *devcfg_wifi_ssid(void)     { return s_ssid; }
const char *devcfg_wifi_password(void) { return s_password; }

void devcfg_set_wifi(const char *ssid, const char *password) {
    if (ssid == NULL)     ssid = "";
    if (password == NULL) password = "";
    strlcpy(s_ssid,     ssid,     sizeof(s_ssid));
    strlcpy(s_password, password, sizeof(s_password));
    save_str(KEY_WIFI_SSID,     s_ssid);
    save_str(KEY_WIFI_PASSWORD, s_password);
    ESP_LOGI(TAG, "wifi cred stored for '%s'", s_ssid);
}

void devcfg_clear_wifi(void) {
    s_ssid[0]     = '\0';
    s_password[0] = '\0';
    save_str(KEY_WIFI_SSID,     "");
    save_str(KEY_WIFI_PASSWORD, "");
}

const char *devcfg_llm_model(void)   { return s_llm_model;   }
const char *devcfg_personality(void) { return s_personality; }

void devcfg_set_llm_model(const char *model) {
    if (model == NULL) model = "";
    strlcpy(s_llm_model, model, sizeof(s_llm_model));
    save_str(KEY_LLM_MODEL, s_llm_model);
}

const char *devcfg_voice_id(void) { return s_voice_id; }

void devcfg_set_voice_id(const char *voice_id) {
    if (voice_id == NULL) voice_id = "";
    strlcpy(s_voice_id, voice_id, sizeof(s_voice_id));
    save_str(KEY_VOICE_ID, s_voice_id);
}

void devcfg_set_personality(const char *persona) {
    if (persona == NULL) persona = "";
    strlcpy(s_personality, persona, sizeof(s_personality));
    save_str(KEY_PERSONALITY, s_personality);
}

const char *devcfg_custom_services(void) {
    // Callers (system prompt builder, /config GET responder) expect a valid
    // JSON array on every read. An empty cache means "user hasn't saved
    // anything yet" — return a literal "[]" so downstream JSON embedding
    // stays well-formed.
    return s_svc_custom[0] ? s_svc_custom : "[]";
}

void devcfg_set_custom_services(const char *json) {
    if (json == NULL) json = "";
    // Over-cap writes are dropped — the caller is expected to have
    // rejected the POST with 413 before getting here. Silent drop is a
    // safety net, not a fallback, so we log it loud.
    if (strlen(json) >= sizeof(s_svc_custom)) {
        ESP_LOGW(TAG, "customServices write rejected: %u > %u",
                 (unsigned)strlen(json), (unsigned)sizeof(s_svc_custom) - 1);
        return;
    }
    strlcpy(s_svc_custom, json, sizeof(s_svc_custom));
    save_str(KEY_SVC_CUSTOM, s_svc_custom);
}

const char *devcfg_services_enabled(void) {
    return s_svc_enabled[0] ? s_svc_enabled : "[]";
}

void devcfg_set_services_enabled(const char *json) {
    if (json == NULL) json = "";
    if (strlen(json) >= sizeof(s_svc_enabled)) {
        ESP_LOGW(TAG, "servicesEnabled write rejected: %u > %u",
                 (unsigned)strlen(json), (unsigned)sizeof(s_svc_enabled) - 1);
        return;
    }
    strlcpy(s_svc_enabled, json, sizeof(s_svc_enabled));
    save_str(KEY_SVC_ENABLED, s_svc_enabled);
}

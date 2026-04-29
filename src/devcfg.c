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
// Pin matches src/board.h (GPIO47 on the BOX-3). Owned exclusively by
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
#define KEY_CREATURE        "creature"
#define KEY_THEME           "theme"

// Number of creature variants the home screen knows how to render. Bump
// this in lockstep with the trait tables in creature_screen.c.
#define CREATURE_COUNT      3

// ---- Built-in default services ---------------------------------------------
// Four x402 endpoints hosted on the Daemon's own seller (daemon-x402s-seven
// .vercel.app). Baked here so a fresh device boots with a working trading-
// intel toolkit instead of an empty service library — the user can still
// add / replace via the on-device wizard or HTTP /config POST. Field
// shape matches what ai.c reads (`id`, `name`, `description`, `baseUrl`,
// `endpoints[].{id, name, method, path, params[]}`).
static const char DEFAULT_CUSTOM_SERVICES_JSON[] =
    "["
      "{"
        "\"id\":\"daemon-fib\","
        "\"name\":\"Fib Levels\","
        "\"description\":\"Fibonacci retracement levels for any major coin on a Binance pair\","
        "\"baseUrl\":\"https://daemon-x402s-seven.vercel.app\","
        "\"endpoints\":[{"
          "\"id\":\"analyze\","
          "\"name\":\"Fib Retracement\","
          "\"method\":\"GET\","
          "\"path\":\"/api/fib\","
          "\"params\":["
            "{\"name\":\"symbol\",\"type\":\"string\",\"description\":\"Token ticker (SOL, BTC, ETH) or full Binance pair (SOLUSDT)\",\"required\":true},"
            "{\"name\":\"timeframe\",\"type\":\"string\",\"description\":\"Candle timeframe (15m, 1h, 4h, 1d). Default 1d.\"},"
            "{\"name\":\"lookback\",\"type\":\"number\",\"description\":\"Candles to scan for the swing. 10..500. Default 90.\"}"
          "]"
        "}]"
      "},"
      "{"
        "\"id\":\"daemon-momentum\","
        "\"name\":\"Momentum\","
        "\"description\":\"RSI MACD and EMA stack with bullish or bearish bias verdict\","
        "\"baseUrl\":\"https://daemon-x402s-seven.vercel.app\","
        "\"endpoints\":[{"
          "\"id\":\"analyze\","
          "\"name\":\"Momentum Read\","
          "\"method\":\"GET\","
          "\"path\":\"/api/momentum\","
          "\"params\":["
            "{\"name\":\"symbol\",\"type\":\"string\",\"description\":\"Token ticker or full Binance pair\",\"required\":true},"
            "{\"name\":\"timeframe\",\"type\":\"string\",\"description\":\"Candle timeframe. Default 1d.\"},"
            "{\"name\":\"lookback\",\"type\":\"number\",\"description\":\"Indicator warmup candles. 50..1000. Default 220.\"}"
          "]"
        "}]"
      "},"
      "{"
        "\"id\":\"daemon-news-pulse\","
        "\"name\":\"News Pulse\","
        "\"description\":\"Fresh headlines summarised into bullish bearish or neutral verdict with confidence\","
        "\"baseUrl\":\"https://daemon-x402s-seven.vercel.app\","
        "\"endpoints\":[{"
          "\"id\":\"analyze\","
          "\"name\":\"News Sentiment\","
          "\"method\":\"GET\","
          "\"path\":\"/api/news-pulse\","
          "\"params\":["
            "{\"name\":\"query\",\"type\":\"string\",\"description\":\"Ticker or topic. Max 200 chars.\",\"required\":true}"
          "]"
        "}]"
      "},"
      "{"
        "\"id\":\"daemon-whale-flow\","
        "\"name\":\"Whale Flow\","
        "\"description\":\"Buy or sell pressure for a Solana SPL token across DEX pairs with accumulation distribution verdict\","
        "\"baseUrl\":\"https://daemon-x402s-seven.vercel.app\","
        "\"endpoints\":[{"
          "\"id\":\"analyze\","
          "\"name\":\"Buy Sell Pressure\","
          "\"method\":\"GET\","
          "\"path\":\"/api/whale-flow\","
          "\"params\":["
            "{\"name\":\"mint\",\"type\":\"string\",\"description\":\"Solana SPL token mint address (32-64 chars base58)\",\"required\":true},"
            "{\"name\":\"timeframe\",\"type\":\"string\",\"description\":\"Window 5m 1h 6h or 24h. Default 1h.\"}"
          "]"
        "}]"
      "}"
    "]";

static const char DEFAULT_SERVICES_ENABLED_JSON[] =
    "[\"daemon-fib\",\"daemon-momentum\",\"daemon-news-pulse\",\"daemon-whale-flow\"]";

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
static uint8_t s_creature_idx = 0;
static uint8_t s_theme        = 0;   // 0 = dark, 1 = light

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
    load_u8  (h, KEY_CREATURE,      &s_creature_idx, 0);
    if (s_creature_idx >= CREATURE_COUNT) s_creature_idx = 0;
    load_u8  (h, KEY_THEME,         &s_theme, 0);
    if (s_theme > 1) s_theme = 0;

    if (h != 0) nvs_close(h);

    // One-shot migration: the deprecated wizard saved its BUILTIN
    // suggestions (SolSignal, Sentinel, CYBERA, DeFi Signal, …) directly
    // into customServices NVS. Those IDs are not in the new toolkit, so
    // they sit dormant in svc_custom and silently override the
    // firmware-baked defaults. If we spot any of them, wipe svc_custom
    // and svc_enabled so the new bake-in defaults activate on first ask.
    // Other NVS keys (wallet, wifi, brightness, …) are untouched.
    static const char *const DEPRECATED_SVC_FINGERPRINTS[] = {
        "\"id\":\"solsignal\"",  "\"id\":\"sentinel\"",
        "\"id\":\"cybera\"",     "\"id\":\"defi-signal\"",
        "\"id\":\"deepblue\"",   "\"id\":\"moonmaker\"",
        "\"id\":\"kerdos\"",     "\"id\":\"xquik\"",
        "\"id\":\"agentfeed\"",  "\"id\":\"gpu-bridge\"",
        "\"id\":\"x402engine\"",
        NULL,
    };
    if (s_svc_custom[0]) {
        for (const char *const *p = DEPRECATED_SVC_FINGERPRINTS; *p; ++p) {
            if (strstr(s_svc_custom, *p)) {
                ESP_LOGI(TAG, "wiping stale chrome-ext services from NVS (matched %s)", *p);
                s_svc_custom[0]  = '\0';
                s_svc_enabled[0] = '\0';
                nvs_handle_t hw;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
                    nvs_erase_key(hw, KEY_SVC_CUSTOM);
                    nvs_erase_key(hw, KEY_SVC_ENABLED);
                    nvs_commit(hw);
                    nvs_close(hw);
                }
                break;
            }
        }
    }

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
    // JSON array on every read. Empty cache = the user hasn't saved their
    // own library yet → fall through to the firmware-baked defaults so the
    // device ships with a working trading-intel toolkit on first boot.
    return s_svc_custom[0] ? s_svc_custom : DEFAULT_CUSTOM_SERVICES_JSON;
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
    // Same default-on-empty story as devcfg_custom_services() — fresh
    // device boots with all four built-in services toggled on.
    return s_svc_enabled[0] ? s_svc_enabled : DEFAULT_SERVICES_ENABLED_JSON;
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

uint8_t devcfg_creature_index(void) {
    return s_creature_idx;
}

void devcfg_set_creature_index(uint8_t idx) {
    if (idx >= CREATURE_COUNT) idx = 0;
    if (idx == s_creature_idx) return;
    s_creature_idx = idx;
    save_u8(KEY_CREATURE, idx);
    ESP_LOGI(TAG, "creature index → %u", (unsigned)idx);
}

devcfg_theme_t devcfg_theme(void) {
    return (devcfg_theme_t)s_theme;
}

void devcfg_set_theme(devcfg_theme_t theme) {
    uint8_t v = (theme == DEVCFG_THEME_LIGHT) ? 1 : 0;
    if (v == s_theme) return;
    s_theme = v;
    save_u8(KEY_THEME, v);
    ESP_LOGI(TAG, "theme → %s", v ? "light" : "dark");
}

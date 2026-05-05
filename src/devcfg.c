// ---------------------------------------------------------------------------
//  NVS-backed device settings + LCD backlight PWM. See devcfg.h.
// ---------------------------------------------------------------------------
#include "devcfg.h"
#include "board.h"
#include "creatures_data.h"

#include <string.h>
#include <time.h>

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
#define KEY_X_ACCESS_TOK    "x_access_tok"
#define KEY_X_REFRESH_TOK   "x_refresh_tok"
#define KEY_X_TOKEN_EXP     "x_token_exp"
#define KEY_X_HANDLE        "x_handle"
#define KEY_X_CLIENT_ID     "x_client_id"
#define KEY_X_REDIRECT_URI  "x_redir_uri"
#define KEY_X_STYLE         "x_style"
#define KEY_PAY_ENABLED     "pay_enabled"
#define KEY_SPND_AUTO_C     "spnd_auto_c"
#define KEY_SPND_CONF_C     "spnd_conf_c"
#define KEY_SPND_DAILY_C    "spnd_daily_c"
#define KEY_SPND_TODAY      "spnd_today"

// CREATURE_DATA_COUNT comes from creatures_data.h — a runtime int sized from
// the array literal, so adding rows in creatures_data.c just works.

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
// pay_enabled_services: JSON blob listing per-service payment permissions.
// Sized to fit a list of ~16 service IDs with their allow/confirm flags.
#define PAY_ENABLED_MAX      512

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

// X (Twitter) OAuth state. Tokens are typically ~150 chars; the x_handle is
// short. Sized generously to absorb any X-side growth without a future migration.
// Client ID + redirect URI are runtime-configurable via the SOCIALS tab so each
// device owner can register their own X dev app without rebuilding firmware.
static char     s_x_access_tok  [512] = {0};
static char     s_x_refresh_tok [512] = {0};
static uint32_t s_x_token_exp         = 0;
static char     s_x_handle      [32]  = {0};
static char     s_x_client_id   [128] = {0};
static char     s_x_redirect_uri[256] = {0};
// Free-form style guide injected into the system prompt when post_to_x is
// available. ~512 chars is enough for a few paragraphs of voice/tone notes.
static char     s_x_style       [512] = {0};

// x402 payment controls ---------------------------------------------------
static char     s_pay_enabled   [PAY_ENABLED_MAX] = {0};
static uint32_t s_spnd_auto_c   = 10;    // $0.10
static uint32_t s_spnd_conf_c   = 500;   // $5.00
static uint32_t s_spnd_daily_c  = 1000;  // $10.00

// Packed blob persisted under KEY_SPND_TODAY.
// micros first: eliminates internal padding on Xtensa (12-byte natural layout
// instead of {int32, 4-byte pad, uint64}).
typedef struct {
    uint64_t micros;    // cumulative spend in micros (1e-6 USD) for that day
    int32_t  utc_day;   // time(NULL)/86400 for the day this counter covers
} spend_today_t;
static spend_today_t s_today = {0, 0};

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

static void load_u32(nvs_handle_t h, const char *key, uint32_t *v, uint32_t fallback) {
    if (h == 0) { *v = fallback; return; }
    if (nvs_get_u32(h, key, v) != ESP_OK) *v = fallback;
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

static void save_u32(const char *key, uint32_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
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

static void save_blob(const char *key, const void *data, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, data, len);
    nvs_commit(h);
    nvs_close(h);
}

// Apply UTC-day rollover to s_today if the wall clock is post-2001 and the
// stored day is in the past. Returns true if a rollover occurred.
#define EPOCH_2001  978307200   // 2001-01-01 00:00:00 UTC as Unix timestamp
static bool maybe_rollover_today(void) {
    time_t now = time(NULL);
    if (now < EPOCH_2001) return false;   // SNTP not yet synced; skip
    int32_t today = (int32_t)(now / 86400);
    if (s_today.utc_day >= today) return false;
    s_today.utc_day = today;
    s_today.micros  = 0;
    save_blob(KEY_SPND_TODAY, &s_today, sizeof(s_today));
    return true;
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
    if (s_creature_idx >= CREATURE_DATA_COUNT) s_creature_idx = 0;
    load_u8  (h, KEY_THEME,         &s_theme, 0);
    if (s_theme > 1) s_theme = 0;
    load_str(h, KEY_X_ACCESS_TOK,    s_x_access_tok,   sizeof(s_x_access_tok));
    load_str(h, KEY_X_REFRESH_TOK,   s_x_refresh_tok,  sizeof(s_x_refresh_tok));
    load_str(h, KEY_X_HANDLE,        s_x_handle,       sizeof(s_x_handle));
    load_str(h, KEY_X_CLIENT_ID,     s_x_client_id,    sizeof(s_x_client_id));
    load_str(h, KEY_X_REDIRECT_URI,  s_x_redirect_uri, sizeof(s_x_redirect_uri));
    load_str(h, KEY_X_STYLE,         s_x_style,        sizeof(s_x_style));
    {
        uint32_t v = 0;
        load_u32(h, KEY_X_TOKEN_EXP, &v, 0);
        s_x_token_exp = v;
    }

    // First-boot default for the redirect URI — points at the bounce page
    // we host. Users only need to override this if they self-host the
    // bounce page on a different domain.
    if (s_x_redirect_uri[0] == '\0') {
        strlcpy(s_x_redirect_uri,
                "https://daemon-x402s-seven.vercel.app/x-callback",
                sizeof(s_x_redirect_uri));
    }

    // x402 payment controls — must be loaded BEFORE nvs_close(h).
    load_str (h, KEY_PAY_ENABLED,  s_pay_enabled,  sizeof(s_pay_enabled));
    load_u32 (h, KEY_SPND_AUTO_C,  &s_spnd_auto_c,  10);
    load_u32 (h, KEY_SPND_CONF_C,  &s_spnd_conf_c,  500);
    load_u32 (h, KEY_SPND_DAILY_C, &s_spnd_daily_c, 1000);
    {
        // Load the today blob. If missing or wrong size, leave defaults (0,0).
        if (h != 0) {
            size_t blen = sizeof(s_today);
            if (nvs_get_blob(h, KEY_SPND_TODAY, &s_today, &blen) != ESP_OK ||
                blen != sizeof(s_today)) {
                s_today.utc_day = 0;
                s_today.micros  = 0;
            }
        }
    }

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
    if (idx >= CREATURE_DATA_COUNT) idx = 0;
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

const char *devcfg_x_access_token(void)  { return s_x_access_tok; }
const char *devcfg_x_refresh_token(void) { return s_x_refresh_tok; }
uint32_t    devcfg_x_token_expiry(void)  { return s_x_token_exp; }
const char *devcfg_x_handle(void)        { return s_x_handle; }

void devcfg_set_x_tokens(const char *access, const char *refresh, uint32_t exp) {
    if (!access)  access  = "";
    if (!refresh) refresh = "";
    strlcpy(s_x_access_tok,  access,  sizeof(s_x_access_tok));
    strlcpy(s_x_refresh_tok, refresh, sizeof(s_x_refresh_tok));
    s_x_token_exp = exp;
    save_str(KEY_X_ACCESS_TOK,  s_x_access_tok);
    save_str(KEY_X_REFRESH_TOK, s_x_refresh_tok);
    save_u32(KEY_X_TOKEN_EXP,   exp);
}

void devcfg_set_x_handle(const char *handle) {
    if (!handle) handle = "";
    strlcpy(s_x_handle, handle, sizeof(s_x_handle));
    save_str(KEY_X_HANDLE, s_x_handle);
}

void devcfg_clear_x(void) {
    s_x_access_tok[0]  = 0;
    s_x_refresh_tok[0] = 0;
    s_x_token_exp      = 0;
    s_x_handle[0]      = 0;
    save_str(KEY_X_ACCESS_TOK,  "");
    save_str(KEY_X_REFRESH_TOK, "");
    save_u32(KEY_X_TOKEN_EXP,   0);
    save_str(KEY_X_HANDLE,      "");
}

bool devcfg_x_connected(void) {
    return s_x_access_tok[0] != 0 && s_x_refresh_tok[0] != 0;
}

const char *devcfg_x_client_id(void)    { return s_x_client_id; }
const char *devcfg_x_redirect_uri(void) { return s_x_redirect_uri; }

bool devcfg_x_configured(void) {
    return s_x_client_id[0] != 0 && s_x_redirect_uri[0] != 0;
}

void devcfg_set_x_client_id(const char *id) {
    if (!id) id = "";
    strlcpy(s_x_client_id, id, sizeof(s_x_client_id));
    save_str(KEY_X_CLIENT_ID, s_x_client_id);
}

void devcfg_set_x_redirect_uri(const char *uri) {
    if (!uri) uri = "";
    strlcpy(s_x_redirect_uri, uri, sizeof(s_x_redirect_uri));
    save_str(KEY_X_REDIRECT_URI, s_x_redirect_uri);
}

const char *devcfg_x_style(void) { return s_x_style; }

void devcfg_set_x_style(const char *style) {
    if (!style) style = "";
    strlcpy(s_x_style, style, sizeof(s_x_style));
    save_str(KEY_X_STYLE, s_x_style);
}

// ---------------------------------------------------------------------------
// x402 payment spending controls
// ---------------------------------------------------------------------------

const char *devcfg_pay_enabled_services(void) {
    // Never return NULL; callers (pay.c, config endpoint) can dereference
    // unconditionally. Empty cache means nothing has been configured yet.
    return s_pay_enabled[0] ? s_pay_enabled : "{\"v\":1,\"items\":[]}";
}

void devcfg_set_pay_enabled_services(const char *json) {
    if (json == NULL) json = "";
    if (strlen(json) >= sizeof(s_pay_enabled)) {
        ESP_LOGW(TAG, "pay_enabled write rejected: %u > %u",
                 (unsigned)strlen(json), (unsigned)sizeof(s_pay_enabled) - 1);
        return;
    }
    strlcpy(s_pay_enabled, json, sizeof(s_pay_enabled));
    save_str(KEY_PAY_ENABLED, s_pay_enabled);
}

uint32_t devcfg_spend_auto_max_cents(void)    { return s_spnd_auto_c; }
uint32_t devcfg_spend_confirm_max_cents(void) { return s_spnd_conf_c; }
uint32_t devcfg_spend_daily_cap_cents(void)   { return s_spnd_daily_c; }

void devcfg_set_spend_auto_max_cents(uint32_t cents) {
    if (cents == s_spnd_auto_c) return;
    s_spnd_auto_c = cents;
    save_u32(KEY_SPND_AUTO_C, cents);
}

void devcfg_set_spend_confirm_max_cents(uint32_t cents) {
    if (cents == s_spnd_conf_c) return;
    s_spnd_conf_c = cents;
    save_u32(KEY_SPND_CONF_C, cents);
}

void devcfg_set_spend_daily_cap_cents(uint32_t cents) {
    if (cents == s_spnd_daily_c) return;
    s_spnd_daily_c = cents;
    save_u32(KEY_SPND_DAILY_C, cents);
}

uint64_t devcfg_spend_today_micros(void) {
    maybe_rollover_today();
    return s_today.micros;
}

void devcfg_add_spend_today_micros(uint64_t delta) {
    maybe_rollover_today();
    s_today.micros += delta;
    save_blob(KEY_SPND_TODAY, &s_today, sizeof(s_today));
}

int32_t devcfg_spend_today_utc_day(void) {
    maybe_rollover_today();
    return s_today.utc_day;
}

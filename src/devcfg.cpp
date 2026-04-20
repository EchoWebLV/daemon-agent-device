#include "devcfg.h"
#include "voice.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static constexpr int PIN_LCD_BL      = 5;
static constexpr int BL_LEDC_CHANNEL = 0;
static constexpr int BL_LEDC_FREQ_HZ = 5000;
static constexpr int BL_LEDC_BITS    = 8;          // 0..255
static constexpr uint8_t MIN_BRIGHTNESS = 12;

// Single shared Preferences handle + mutex. Every getter/setter opens
// and closes `s_nvs` around a single NVS op; without this mutex the
// handle is raced across ~7 tasks (main loop, LLM worker, xpost worker,
// memory task, HTTP server, touch handler, serial input). Concurrent
// begin()/end() calls corrupt nvs internal state and trigger a
// Guru Meditation / task WDT reset, which is the random-restart
// symptom we were chasing.
static Preferences       s_nvs;
static SemaphoreHandle_t s_nvsMutex = nullptr;

static SemaphoreHandle_t nvsMutex() {
  // Lazily create on first call so callers during static init (e.g.
  // an unlikely pre-setup() access) still work.
  if (!s_nvsMutex) {
    static portMUX_TYPE initLock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&initLock);
    if (!s_nvsMutex) s_nvsMutex = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&initLock);
  }
  return s_nvsMutex;
}

// RAII lock — holds the mutex for the lifetime of a devcfg getter/setter.
// Uses a 2 s timeout so a genuinely stuck caller surfaces as a log line
// instead of deadlocking the entire device.
struct NvsLock {
  bool held_;
  NvsLock() : held_(false) {
    SemaphoreHandle_t m = nvsMutex();
    if (!m) return;
    held_ = (xSemaphoreTake(m, pdMS_TO_TICKS(2000)) == pdTRUE);
    if (!held_) {
      Serial.println("devcfg: NVS mutex timeout (proceeding unlocked — possible race)");
    }
  }
  ~NvsLock() {
    if (held_) xSemaphoreGive(nvsMutex());
  }
};

static uint8_t  s_volume     = 21;
static uint8_t  s_brightness = 255;
static bool     s_bluetooth  = false;
static uint8_t  s_faceStyle  = 0;

// X/Twitter OAuth credentials — cached in RAM because several hot paths
// (statusicons at 60 Hz, settingsscreen at 60 Hz) poll them every frame.
// Without the cache we hammered NVS 240x/s and Preferences.cpp logs an
// ERROR on every NOT_FOUND miss — that was enough to heat the board up
// and trigger occasional watchdog resets. Loaded once in devcfgBegin(),
// kept in sync by the devcfgSetX*() setters below.
static String   s_xApiKey;
static String   s_xApiSecret;
static String   s_xAccessToken;
static String   s_xAccessTokenSecret;
static bool     s_xCredsPresent = false;

static void recomputeXCredsPresent() {
  s_xCredsPresent = s_xApiKey.length()            > 0 &&
                    s_xApiSecret.length()         > 0 &&
                    s_xAccessToken.length()       > 0 &&
                    s_xAccessTokenSecret.length() > 0;
}

static void applyBrightness(uint8_t b) {
  if (b < MIN_BRIGHTNESS) b = MIN_BRIGHTNESS;
  ledcWrite(BL_LEDC_CHANNEL, b);
}

void devcfgBegin() {
  // Ensure the NVS mutex exists before any other task can race us.
  nvsMutex();

  // LEDC PWM for the LCD backlight.
  ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ_HZ, BL_LEDC_BITS);
  ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);

  // Load persisted values (with sensible defaults on first boot).
  {
    NvsLock lk;
    s_nvs.begin("daemon", /*readOnly=*/false);
    s_volume     = s_nvs.getUChar("vol",   21);
    s_brightness = s_nvs.getUChar("bri",   255);
    s_bluetooth  = s_nvs.getBool ("bt",    false);
    s_faceStyle  = s_nvs.getUChar("face",  0);
    if (s_faceStyle >= DEVCFG_FACE_COUNT) s_faceStyle = 0;

    // Pre-cache the 4 X OAuth keys. isKey() gates the getString() so
    // the NOT_FOUND path doesn't log an ESP error per missing key on
    // first boot; once present in NVS they just read normally.
    s_xApiKey            = s_nvs.isKey("xo_ck") ? s_nvs.getString("xo_ck", "") : String();
    s_xApiSecret         = s_nvs.isKey("xo_cs") ? s_nvs.getString("xo_cs", "") : String();
    s_xAccessToken       = s_nvs.isKey("xo_tk") ? s_nvs.getString("xo_tk", "") : String();
    s_xAccessTokenSecret = s_nvs.isKey("xo_ts") ? s_nvs.getString("xo_ts", "") : String();
    recomputeXCredsPresent();

    s_nvs.end();
  }

  applyBrightness(s_brightness);
  voiceSetVolume(s_volume);

  Serial.printf("devcfg: vol=%u bri=%u bt=%d\n",
                s_volume, s_brightness, s_bluetooth ? 1 : 0);
}

uint8_t devcfgVolume()      { return s_volume; }
uint8_t devcfgBrightness()  { return s_brightness; }
bool    devcfgBluetooth()   { return s_bluetooth; }
uint8_t devcfgFaceStyle()   { return s_faceStyle; }

void devcfgSetFaceStyle(uint8_t s) {
  if (s >= DEVCFG_FACE_COUNT) s = 0;
  if (s == s_faceStyle) return;
  s_faceStyle = s;
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("face", s);
  s_nvs.end();
}

void devcfgSetVolume(uint8_t v) {
  if (v > 21) v = 21;
  if (v == s_volume) return;
  s_volume = v;
  voiceSetVolume(v);
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("vol", v);
  s_nvs.end();
}

void devcfgSetBrightness(uint8_t b) {
  if (b == s_brightness) return;
  s_brightness = b;
  applyBrightness(b);
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("bri", b);
  s_nvs.end();
}

void devcfgSetBluetooth(bool on) {
  if (on == s_bluetooth) return;
  s_bluetooth = on;
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("bt", on);
  s_nvs.end();
  // NOTE: BLE stack not yet wired up; this just persists the flag.
}

String devcfgWifiSSID() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("wf_ssid", "");
  s_nvs.end();
  return v;
}
String devcfgWifiPassword() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("wf_pass", "");
  s_nvs.end();
  return v;
}
void devcfgSetWifi(const String &ssid, const String &password) {
  {
    NvsLock lk;
    s_nvs.begin("daemon", false);
    s_nvs.putString("wf_ssid", ssid);
    s_nvs.putString("wf_pass", password);
    s_nvs.end();
  }
  Serial.printf("devcfg: stored wifi cred for '%s'\n", ssid.c_str());
}
void devcfgClearWifi() {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.remove("wf_ssid");
  s_nvs.remove("wf_pass");
  s_nvs.end();
}

// ---- LLM model + personality + services ---------------------------------
String devcfgLlmModel() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("llm_model", "google/gemini-3.1-pro");
  s_nvs.end();
  return v;
}
void devcfgSetLlmModel(const String &m) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("llm_model", m);
  s_nvs.end();
}

String devcfgPersonality() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("persona", "");
  s_nvs.end();
  return v;
}
void devcfgSetPersonality(const String &p) {
  // NVS string entries are limited to ~4000 bytes; personalities this
  // long would be silly but we still clamp as a safety net.
  String clamped = p;
  if (clamped.length() > 3800) clamped.remove(3800);
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("persona", clamped);
  s_nvs.end();
}

String devcfgServicesEnabled() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("svc_en", "[]");
  s_nvs.end();
  return v;
}
void devcfgSetServicesEnabled(const String &jsonArray) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("svc_en", jsonArray);
  s_nvs.end();
}

String devcfgCustomServices() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("svc_cu", "[]");
  s_nvs.end();
  return v;
}
void devcfgSetCustomServices(const String &jsonArray) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("svc_cu", jsonArray);
  s_nvs.end();
}

// ---- On-chain memory -----------------------------------------------------
bool devcfgMemoryEnabled() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("mem_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetMemoryEnabled(bool on) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("mem_on", on);
  s_nvs.end();
}

// ---- Arweave (Irys) archive ----------------------------------------------
bool devcfgArweaveEnabled() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("ar_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetArweaveEnabled(bool on) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("ar_on", on);
  // Unified policy: when the user flips the single "MEMORY" toggle on,
  // we stop writing Solana memo txs so they don't keep paying for a
  // backend the UI no longer exposes. Advanced users can still flip the
  // memo backend back on via POST /config if they want redundancy.
  if (on) s_nvs.putBool("mem_on", false);
  s_nvs.end();
}

// ---- Heartbeat ------------------------------------------------------------
bool devcfgHeartbeatEnabled() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("hb_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetHeartbeatEnabled(bool on) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("hb_on", on);
  s_nvs.end();
}
String devcfgHeartbeatPrompt() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("hb_prompt", "");
  s_nvs.end();
  return v;
}
void devcfgSetHeartbeatPrompt(const String &p) {
  String clamped = p;
  if (clamped.length() > 1200) clamped.remove(1200);
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("hb_prompt", clamped);
  s_nvs.end();
}
uint32_t devcfgHeartbeatIntervalMin() {
  uint32_t v;
  {
    NvsLock lk;
    s_nvs.begin("daemon", true);
    v = s_nvs.getUInt("hb_min", 5);   // default 5 min
    s_nvs.end();
  }
  if (v < 1)    v = 1;
  if (v > 1440) v = 1440;             // cap at 24h
  return v;
}
void devcfgSetHeartbeatIntervalMin(uint32_t minutes) {
  if (minutes < 1)    minutes = 1;
  if (minutes > 1440) minutes = 1440;
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putUInt("hb_min", minutes);
  s_nvs.end();
}

// ---- X (Twitter) auto-poster --------------------------------------------
// NVS key budget: 15-char limit on keys under the Arduino Preferences
// wrapper, so we use compact "xp_*" and "xo_*" namespacing.
bool devcfgXPostEnabled() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("xp_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetXPostEnabled(bool on) {
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("xp_on", on);
  s_nvs.end();
}

String devcfgXPostPrompt() {
  NvsLock lk;
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("xp_prompt", "");
  s_nvs.end();
  return v;
}
void devcfgSetXPostPrompt(const String &p) {
  String clamped = p;
  if (clamped.length() > 1200) clamped.remove(1200);
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putString("xp_prompt", clamped);
  s_nvs.end();
}

uint32_t devcfgXPostIntervalMin() {
  uint32_t v;
  {
    NvsLock lk;
    s_nvs.begin("daemon", true);
    v = s_nvs.getUInt("xp_min", 60);
    s_nvs.end();
  }
  // X free/pay-per-use tier caps at ~17 writes/24h, so we enforce a 15 min
  // floor both as a courtesy to the user's daily quota and to keep posts
  // spaced far enough that duplicate-detection on X doesn't reject them.
  if (v < 15)   v = 15;
  if (v > 1440) v = 1440;
  return v;
}
void devcfgSetXPostIntervalMin(uint32_t minutes) {
  if (minutes < 15)   minutes = 15;
  if (minutes > 1440) minutes = 1440;
  NvsLock lk;
  s_nvs.begin("daemon", false);
  s_nvs.putUInt("xp_min", minutes);
  s_nvs.end();
}

// All four X getters read from the RAM cache populated in devcfgBegin()
// and kept in sync by the setters below. This used to hit NVS on every
// call, which was fatal because two hot paths (status-bar icons and the
// settings screen tick) polled them at 60 Hz — the resulting 240 NVS
// reads/s + 240 "NOT_FOUND" error prints/s warmed the board and
// occasionally stalled long enough to trip a watchdog.
String devcfgXApiKey()            { return s_xApiKey; }
String devcfgXApiSecret()         { return s_xApiSecret; }
String devcfgXAccessToken()       { return s_xAccessToken; }
String devcfgXAccessTokenSecret() { return s_xAccessTokenSecret; }

void devcfgSetXApiKey(const String &v) {
  s_xApiKey = v;
  recomputeXCredsPresent();
  NvsLock lk;
  s_nvs.begin("daemon", false); s_nvs.putString("xo_ck", v); s_nvs.end();
}
void devcfgSetXApiSecret(const String &v) {
  s_xApiSecret = v;
  recomputeXCredsPresent();
  NvsLock lk;
  s_nvs.begin("daemon", false); s_nvs.putString("xo_cs", v); s_nvs.end();
}
void devcfgSetXAccessToken(const String &v) {
  s_xAccessToken = v;
  recomputeXCredsPresent();
  NvsLock lk;
  s_nvs.begin("daemon", false); s_nvs.putString("xo_tk", v); s_nvs.end();
}
void devcfgSetXAccessTokenSecret(const String &v) {
  s_xAccessTokenSecret = v;
  recomputeXCredsPresent();
  NvsLock lk;
  s_nvs.begin("daemon", false); s_nvs.putString("xo_ts", v); s_nvs.end();
}

bool devcfgXCredentialsPresent() { return s_xCredsPresent; }

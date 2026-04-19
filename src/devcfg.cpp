#include "devcfg.h"
#include "voice.h"

#include <Preferences.h>

static constexpr int PIN_LCD_BL      = 5;
static constexpr int BL_LEDC_CHANNEL = 0;
static constexpr int BL_LEDC_FREQ_HZ = 5000;
static constexpr int BL_LEDC_BITS    = 8;          // 0..255
static constexpr uint8_t MIN_BRIGHTNESS = 12;

static Preferences s_nvs;
static uint8_t  s_volume     = 21;
static uint8_t  s_brightness = 255;
static bool     s_bluetooth  = false;
static uint8_t  s_faceStyle  = 0;

static void applyBrightness(uint8_t b) {
  if (b < MIN_BRIGHTNESS) b = MIN_BRIGHTNESS;
  ledcWrite(BL_LEDC_CHANNEL, b);
}

void devcfgBegin() {
  // LEDC PWM for the LCD backlight.
  ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ_HZ, BL_LEDC_BITS);
  ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);

  // Load persisted values (with sensible defaults on first boot).
  s_nvs.begin("daemon", /*readOnly=*/false);
  s_volume     = s_nvs.getUChar("vol",   21);
  s_brightness = s_nvs.getUChar("bri",   255);
  s_bluetooth  = s_nvs.getBool ("bt",    false);
  s_faceStyle  = s_nvs.getUChar("face",  0);
  if (s_faceStyle >= DEVCFG_FACE_COUNT) s_faceStyle = 0;
  s_nvs.end();

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
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("face", s);
  s_nvs.end();
}

void devcfgSetVolume(uint8_t v) {
  if (v > 21) v = 21;
  if (v == s_volume) return;
  s_volume = v;
  voiceSetVolume(v);
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("vol", v);
  s_nvs.end();
}

void devcfgSetBrightness(uint8_t b) {
  if (b == s_brightness) return;
  s_brightness = b;
  applyBrightness(b);
  s_nvs.begin("daemon", false);
  s_nvs.putUChar("bri", b);
  s_nvs.end();
}

void devcfgSetBluetooth(bool on) {
  if (on == s_bluetooth) return;
  s_bluetooth = on;
  s_nvs.begin("daemon", false);
  s_nvs.putBool("bt", on);
  s_nvs.end();
  // NOTE: BLE stack not yet wired up; this just persists the flag.
}

String devcfgWifiSSID() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("wf_ssid", "");
  s_nvs.end();
  return v;
}
String devcfgWifiPassword() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("wf_pass", "");
  s_nvs.end();
  return v;
}
void devcfgSetWifi(const String &ssid, const String &password) {
  s_nvs.begin("daemon", false);
  s_nvs.putString("wf_ssid", ssid);
  s_nvs.putString("wf_pass", password);
  s_nvs.end();
  Serial.printf("devcfg: stored wifi cred for '%s'\n", ssid.c_str());
}
void devcfgClearWifi() {
  s_nvs.begin("daemon", false);
  s_nvs.remove("wf_ssid");
  s_nvs.remove("wf_pass");
  s_nvs.end();
}

// ---- LLM model + personality + services ---------------------------------
String devcfgLlmModel() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("llm_model", "google/gemini-3.1-pro");
  s_nvs.end();
  return v;
}
void devcfgSetLlmModel(const String &m) {
  s_nvs.begin("daemon", false);
  s_nvs.putString("llm_model", m);
  s_nvs.end();
}

String devcfgPersonality() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("persona", "");
  s_nvs.end();
  return v;
}
void devcfgSetPersonality(const String &p) {
  s_nvs.begin("daemon", false);
  // NVS string entries are limited to ~4000 bytes; personalities this
  // long would be silly but we still clamp as a safety net.
  String clamped = p;
  if (clamped.length() > 3800) clamped.remove(3800);
  s_nvs.putString("persona", clamped);
  s_nvs.end();
}

String devcfgServicesEnabled() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("svc_en", "[]");
  s_nvs.end();
  return v;
}
void devcfgSetServicesEnabled(const String &jsonArray) {
  s_nvs.begin("daemon", false);
  s_nvs.putString("svc_en", jsonArray);
  s_nvs.end();
}

String devcfgCustomServices() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("svc_cu", "[]");
  s_nvs.end();
  return v;
}
void devcfgSetCustomServices(const String &jsonArray) {
  s_nvs.begin("daemon", false);
  s_nvs.putString("svc_cu", jsonArray);
  s_nvs.end();
}

// ---- On-chain memory -----------------------------------------------------
bool devcfgMemoryEnabled() {
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("mem_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetMemoryEnabled(bool on) {
  s_nvs.begin("daemon", false);
  s_nvs.putBool("mem_on", on);
  s_nvs.end();
}

// ---- Arweave (Irys) archive ----------------------------------------------
bool devcfgArweaveEnabled() {
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("ar_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetArweaveEnabled(bool on) {
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
  s_nvs.begin("daemon", true);
  bool v = s_nvs.getBool("hb_on", false);
  s_nvs.end();
  return v;
}
void devcfgSetHeartbeatEnabled(bool on) {
  s_nvs.begin("daemon", false);
  s_nvs.putBool("hb_on", on);
  s_nvs.end();
}
String devcfgHeartbeatPrompt() {
  s_nvs.begin("daemon", true);
  String v = s_nvs.getString("hb_prompt", "");
  s_nvs.end();
  return v;
}
void devcfgSetHeartbeatPrompt(const String &p) {
  String clamped = p;
  if (clamped.length() > 1200) clamped.remove(1200);
  s_nvs.begin("daemon", false);
  s_nvs.putString("hb_prompt", clamped);
  s_nvs.end();
}
uint32_t devcfgHeartbeatIntervalMin() {
  s_nvs.begin("daemon", true);
  uint32_t v = s_nvs.getUInt("hb_min", 5);   // default 5 min
  s_nvs.end();
  if (v < 1)    v = 1;
  if (v > 1440) v = 1440;                    // cap at 24h
  return v;
}
void devcfgSetHeartbeatIntervalMin(uint32_t minutes) {
  if (minutes < 1)    minutes = 1;
  if (minutes > 1440) minutes = 1440;
  s_nvs.begin("daemon", false);
  s_nvs.putUInt("hb_min", minutes);
  s_nvs.end();
}

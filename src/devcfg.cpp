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
  s_nvs.end();

  applyBrightness(s_brightness);
  voiceSetVolume(s_volume);

  Serial.printf("devcfg: vol=%u bri=%u bt=%d\n",
                s_volume, s_brightness, s_bluetooth ? 1 : 0);
}

uint8_t devcfgVolume()      { return s_volume; }
uint8_t devcfgBrightness()  { return s_brightness; }
bool    devcfgBluetooth()   { return s_bluetooth; }

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

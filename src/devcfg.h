// ============================================================================
//  Device-level settings: volume, LCD brightness, Bluetooth toggle.
//  All values persist across reboots in NVS (ESP32 Preferences API) and
//  are applied immediately to the relevant hardware on set.
// ============================================================================
#pragma once
#include <Arduino.h>

void    devcfgBegin();                  // load from NVS + apply to HW

// Volume: 0..21 (forwarded to the ESP32-audioI2S library).
uint8_t devcfgVolume();
void    devcfgSetVolume(uint8_t v);

// Brightness: 0..255 (PWM duty on the LCD backlight pin). Clamped to
// a small minimum so the screen is never completely black.
uint8_t devcfgBrightness();
void    devcfgSetBrightness(uint8_t b);

// Bluetooth toggle. Today this just flips a persisted flag; no BLE stack
// is started yet. Hook up when a real feature needs it.
bool    devcfgBluetooth();
void    devcfgSetBluetooth(bool on);

// Face style picker.
//   0 = Daemon     (neon almond eyes — default)
//   1 = Robot      (circular eyes with pupils)
//   2 = Toy Robot  (blue chassis + red antenna + grille mouth)
//   3 = Calculator ( = | analog LCD / 7-segment look)
// Swipe right on the creature screen cycles through them.
static constexpr uint8_t DEVCFG_FACE_COUNT = 4;
uint8_t devcfgFaceStyle();
void    devcfgSetFaceStyle(uint8_t s);

// Wi-Fi credentials. Empty string means "use the compile-time fallback
// from secrets.h" (only matters on first boot). Setting these persists
// the new SSID/password so the user can change networks from the UI.
String  devcfgWifiSSID();
String  devcfgWifiPassword();
void    devcfgSetWifi(const String &ssid, const String &password);
void    devcfgClearWifi();

// LLM model id (uses the same provider/name format as the Chrome
// extension, e.g. "google/gemini-3.1-pro", "openai/gpt-4o-mini").
String  devcfgLlmModel();
void    devcfgSetLlmModel(const String &m);

// Custom personality — injected into the AI system prompt in place of the
// default "You are Daemon..." text. Empty string means "use default".
String  devcfgPersonality();
void    devcfgSetPersonality(const String &p);

// JSON arrays stored as strings (so we can ship them straight to the web
// UI). Contents:
//   svc_enabled: array of service ids the user has flipped on
//   svc_custom:  array of full X402Service JSON objects added via .md
String  devcfgServicesEnabled();
void    devcfgSetServicesEnabled(const String &jsonArray);
String  devcfgCustomServices();
void    devcfgSetCustomServices(const String &jsonArray);

// ---- On-chain memory (encrypted memo log on Solana mainnet) ---------------
// When enabled, each completed user ↔ assistant exchange is encrypted
// (AES-256-GCM with a key derived from the wallet seed) and written as a
// Solana memo transaction, so Daemon remembers conversations across
// reboots. Costs ~5000 lamports (~$0.0003) per exchange in SOL.
bool    devcfgMemoryEnabled();
void    devcfgSetMemoryEnabled(bool on);

// ---- Arweave/Irys archive ------------------------------------------------
// When on, every memory write ALSO uploads the same encrypted blob to
// Arweave via Irys. Under 100 KiB uploads are free, so chat memos add
// zero cost but gain permanent decentralized storage. Requires memory
// to be enabled (the encryption pipeline is shared).
bool    devcfgArweaveEnabled();
void    devcfgSetArweaveEnabled(bool on);

// ---- Heartbeat (scheduled prompt) -----------------------------------------
// When enabled, the device runs `heartbeatPrompt` through the normal
// aiAskOneShot + TTS pipeline every `heartbeatIntervalMin` minutes. Each
// run costs USDC like any other chat. All fields persist across reboots.
bool     devcfgHeartbeatEnabled();
void     devcfgSetHeartbeatEnabled(bool on);
String   devcfgHeartbeatPrompt();
void     devcfgSetHeartbeatPrompt(const String &p);
uint32_t devcfgHeartbeatIntervalMin();
void     devcfgSetHeartbeatIntervalMin(uint32_t minutes);

// ---- X (Twitter) auto-poster ---------------------------------------------
// Same pattern as Heartbeat but the generated text is POSTed to the X API
// v2 (POST /2/tweets) using user-supplied OAuth 1.0a credentials. The user
// must create a project + app at developer.x.com, enable Read+Write, and
// paste all four keys into the web portal. Each successful tweet is billed
// by X at their current pay-per-use rate (~$0.01).
bool     devcfgXPostEnabled();
void     devcfgSetXPostEnabled(bool on);
String   devcfgXPostPrompt();
void     devcfgSetXPostPrompt(const String &p);
uint32_t devcfgXPostIntervalMin();              // >= 15
void     devcfgSetXPostIntervalMin(uint32_t minutes);

// OAuth 1.0a credentials. Returned verbatim by getters (firmware needs
// them to sign). `GET /config` in the web server only reports presence
// booleans, never the values.
String   devcfgXApiKey();
String   devcfgXApiSecret();
String   devcfgXAccessToken();
String   devcfgXAccessTokenSecret();
void     devcfgSetXApiKey(const String &v);
void     devcfgSetXApiSecret(const String &v);
void     devcfgSetXAccessToken(const String &v);
void     devcfgSetXAccessTokenSecret(const String &v);

// True iff all four OAuth credential fields are non-empty.
bool     devcfgXCredentialsPresent();

// ---- Moltbook (moltbook.com) auto-poster ---------------------------------
// Posts to the AI-agent social network. Uses a simple Bearer token (API key)
// obtained during agent registration. Rate limit: 1 post per 30 minutes.
bool     devcfgMoltbookEnabled();
void     devcfgSetMoltbookEnabled(bool on);
String   devcfgMoltbookPrompt();
void     devcfgSetMoltbookPrompt(const String &p);
uint32_t devcfgMoltbookIntervalMin();              // >= 30
void     devcfgSetMoltbookIntervalMin(uint32_t minutes);
String   devcfgMoltbookApiKey();
void     devcfgSetMoltbookApiKey(const String &v);

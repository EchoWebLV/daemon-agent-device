// ============================================================================
//  voice.cpp — ELEVENLABS-PRIMARY + PIPER-FALLBACK VARIANT.
//
//  This file is NOT compiled in-place. It lives in /variants/ so PlatformIO
//  ignores it. To activate, copy it over src/voice.cpp:
//
//      cp variants/voice_piper.cpp src/voice.cpp
//
//  …and then `pio run -t upload`.
//
//  What this variant does:
//    - Tries ElevenLabs FIRST on every utterance — higher-quality voice,
//      paid per credit, needs internet. Uses the TLS keep-alive path so
//      repeat utterances skip the ~800 ms handshake.
//    - Falls back to the local Piper HTTP server (PIPER_HOST/PIPER_PORT in
//      secrets.h) if ElevenLabs is unreachable, rate-limited, or out of
//      credits. A 30 s per-provider backoff keeps a bad state from eating
//      a timeout on every utterance.
//    - Two-slot file architecture + keep-alive on both clients.
//
//  Companion variant: variants/voice_elevenlabs.cpp  — ElevenLabs only,
//  no Piper code at all. Swap to it when you never want the Piper path
//  compiled in (e.g. to save flash / RAM).
// ============================================================================
#include "voice.h"
#include "secrets.h"
#include "netgate.h"

#include <Audio.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------------------------------------------------------------------
// I2S pins (PCM5101 on the Waveshare 2.8")
// ---------------------------------------------------------------------------
static constexpr int I2S_BCLK = 48;
static constexpr int I2S_LRC  = 38;
static constexpr int I2S_DOUT = 47;

// Synthesized audio is cached into LittleFS and played back through the
// Audio library's proven connecttoFS path. The streaming-direct-to-
// decoder patch had a premature EOF bug in the library's chunked reader
// that cut audio off after ~6 KB, so we stage the full response on-disk
// first and then let the library do what it's good at.
//
// Two alternating slot indices instead of one file: a new utterance can
// be downloaded into the idle slot while the other slot is still being
// read by the decoder. That eliminates the ~530 ms "stop + wait for
// LittleFS handle close" stall and lets the fetch overlap with the tail
// of the currently-playing reply.
//
// Each slot's path is tracked as a full String so the extension can
// differ between utterances — Piper returns WAV, ElevenLabs returns MP3,
// and the Audio library dispatches codec strictly by file extension
// (`.wav` vs `.mp3`).
static constexpr int SLOT_COUNT = 2;
static String        s_slotPath[SLOT_COUNT] = { "/tts_a.mp3", "/tts_b.mp3" };
static int           s_playbackIdx = 0;   // decoder is holding this slot
static int           s_fetchIdx    = 1;   // next write target

static Audio       *s_audio      = nullptr;
static TaskHandle_t s_audioTask  = nullptr;
static TaskHandle_t s_fetchTask  = nullptr;
static QueueHandle_t s_speakQueue = nullptr;
static volatile bool s_ready     = false;
static volatile bool s_playing   = false;
// True while the fetch task holds a TLS session to ElevenLabs and/or is
// writing to LittleFS. Flipped on at dequeue, off after connecttoFS
// completes (whether or not the fetch succeeded).
static volatile bool s_fetching  = false;

// Reused HTTPS client + HTTPClient for ElevenLabs. mbedTLS contexts are
// ~40 KB each and the handshake to api.elevenlabs.io is ~500–800 ms on
// the ESP32-S3, so keeping a single connection alive across utterances
// saves both heap churn and a big chunk of wall-clock latency.
// setReuse(true) + HTTP/1.1 keep-alive means the second utterance onward
// skips the handshake entirely.
static WiFiClientSecure *s_httpsClient = nullptr;
static HTTPClient        s_http;

// Reused plaintext client for the local Piper server on the LAN. No TLS,
// no reverse proxy, no internet hop — typical round-trip is 200–400 ms
// for a full WAV back, vs 1–3 s for ElevenLabs' remote synthesis.
static WiFiClient *s_piperClient = nullptr;
static HTTPClient  s_piperHttp;
// When Piper has errored recently we briefly skip it and go straight to
// ElevenLabs, so a dead/disconnected Mac server doesn't add seconds of
// latency to every utterance. Reset automatically on success.
static uint32_t    s_piperBackoffUntil = 0;

// Queue job — carries the full utterance text so the fetch runs off the
// main loop. We allocate the text inline (C string) so the FreeRTOS queue
// can memcpy it without chasing heap pointers.
struct SpeakJob {
  char text[1400];        // ElevenLabs text cap; longer is truncated
};

// ---------------------------------------------------------------------------
// Boot-time direct-I2S beep (hardware probe)
// ---------------------------------------------------------------------------
static constexpr int BEEP_SR = 22050;

static bool beepInstall() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = BEEP_SR;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = 4;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;
  cfg.fixed_mclk           = 0;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_BCLK;
  pins.ws_io_num    = I2S_LRC;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

static void beepTone(uint16_t freq, uint16_t durationMs, int16_t amp) {
  const int totalFrames = (BEEP_SR * durationMs) / 1000;
  if (totalFrames <= 0) return;
  const float phaseInc = 2.0f * (float)PI * (float)freq / (float)BEEP_SR;
  float phase = 0.0f;
  const int attack  = min(220, totalFrames / 6);
  const int release = min(500, totalFrames / 3);
  int16_t buf[128];
  size_t  written;
  int     idx = 0;
  for (int s = 0; s < totalFrames; ++s) {
    float env = 1.0f;
    if (s < attack)                     env = (float)s / (float)attack;
    else if (s > totalFrames - release) env = (float)(totalFrames - s) / (float)release;
    int16_t v = (int16_t)(sinf(phase) * (float)amp * env);
    phase += phaseInc;
    if (phase > 2.0f * (float)PI) phase -= 2.0f * (float)PI;
    buf[idx * 2]     = v;
    buf[idx * 2 + 1] = v;
    idx++;
    if (idx >= 64) {
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
      idx = 0;
    }
  }
  if (idx > 0) i2s_write(I2S_NUM_0, buf, idx * 4, &written, portMAX_DELAY);
}

// ---------------------------------------------------------------------------
// Audio task — pumps the MP3 decoder.
// ---------------------------------------------------------------------------
static void audioTaskEntry(void *) {
  for (;;) {
    if (s_audio) s_audio->loop();
    if (s_playing && s_audio && !s_audio->isRunning()) s_playing = false;
    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------------------
// Fetch task — runs the blocking TTS POST + LittleFS write off the main
// Arduino loop so the UI keeps ticking while Daemon fetches his reply
// audio. ElevenLabs is the preferred voice (higher quality); Piper on
// the LAN is the fallback that kicks in if ElevenLabs is unreachable,
// out of credits, or rate-limited.
// ---------------------------------------------------------------------------
static bool fetchMp3FromElevenLabs(const String &text, const char *outPath);
static bool fetchWavFromPiper     (const String &text, const char *outPath);

// When either provider has errored recently we briefly skip it, so a
// dead server / blown credit balance doesn't pile the timeout onto every
// utterance. Reset on success. Both counters use the same 30 s window
// because the failure modes are similar: transient for ElevenLabs
// (network hiccup, rate limit, provider blip), transient for Piper
// (Mac asleep, server crashed, hotspot reset).
static constexpr uint32_t ELEVENLABS_BACKOFF_MS = 30000;
static constexpr uint32_t PIPER_BACKOFF_MS      = 30000;
static uint32_t           s_elevenlabsBackoffUntil = 0;

static void fetchTaskEntry(void *) {
  for (;;) {
    SpeakJob job;
    if (xQueueReceive(s_speakQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    if (!s_ready || !s_audio) continue;

    String text = String(job.text);
    if (text.length() == 0) continue;

    // Guard flag so other HTTPS-using tasks (Arweave, memory) don't open
    // a second TLS session on top of ours — two concurrent mbedTLS
    // contexts easily OOM on the ESP32-S3.
    s_fetching = true;

    // Fetch into the *idle* slot — the decoder may still be playing out
    // of s_slotPath[s_playbackIdx]. Writing to a different file avoids
    // the LittleFS `lfs_mlist_isopen` assert entirely and lets the fetch
    // overlap with the tail of the previous utterance.
    const int  target   = s_fetchIdx;
    const char slotChar = (char)('a' + target);

    uint32_t t0 = millis();

    String chosenPath;
    bool   okFetch  = false;
    bool   usedPiper = false;

    // Primary: ElevenLabs. Only skipped if a recent call has failed —
    // otherwise we always pay the ~800 ms first-boot TLS handshake once
    // in exchange for the higher-quality voice.
    bool elevenlabsSkipped = (millis() < s_elevenlabsBackoffUntil);
    if (!elevenlabsSkipped) {
      chosenPath = String("/tts_") + slotChar + ".mp3";
      okFetch    = fetchMp3FromElevenLabs(text, chosenPath.c_str());
      if (okFetch) {
        s_elevenlabsBackoffUntil = 0;
      } else {
        s_elevenlabsBackoffUntil = millis() + ELEVENLABS_BACKOFF_MS;
        Serial.println("voice: elevenlabs failed — backing off, falling back to Piper");
      }
    }

    // Fallback: local Piper on the LAN. Kicks in when ElevenLabs is
    // unreachable / in backoff, or not configured at all.
    if (!okFetch && strlen(PIPER_HOST) > 0 &&
        millis() >= s_piperBackoffUntil) {
      chosenPath = String("/tts_") + slotChar + ".wav";
      okFetch    = fetchWavFromPiper(text, chosenPath.c_str());
      usedPiper  = okFetch;
      if (okFetch) {
        s_piperBackoffUntil = 0;
      } else {
        s_piperBackoffUntil = millis() + PIPER_BACKOFF_MS;
        Serial.println("voice: piper fallback also failed — giving up this utterance");
      }
    }

    if (!okFetch) { s_fetching = false; continue; }

    // Clean up the stale other-codec file in this slot so flash doesn't
    // slowly accumulate orphaned /tts_*.mp3 and /tts_*.wav pairs.
    String stale = chosenPath.endsWith(".wav")
                     ? String("/tts_") + slotChar + ".mp3"
                     : String("/tts_") + slotChar + ".wav";
    if (LittleFS.exists(stale)) LittleFS.remove(stale);

    uint32_t tFetched = millis();

    // Fetch succeeded — hand the new file to the decoder. connecttoFS
    // internally stops the current song and closes its file handle, so
    // we don't need the old stopSong-then-wait dance anymore.
    bool ok = s_audio->connecttoFS(LittleFS, chosenPath.c_str());
    if (ok) {
      s_slotPath[target] = chosenPath;
      s_playbackIdx      = target;
      s_fetchIdx         = 1 - target;
    }
    s_playing  = ok;
    s_fetching = false;
    Serial.printf("voice: %s fetch=%lums swap=%lums ok=%d -> %s\n",
                  usedPiper ? "piper" : "elevenlabs",
                  (unsigned long)(tFetched - t0),
                  (unsigned long)(millis() - tFetched),
                  ok ? 1 : 0, chosenPath.c_str());
    if (!ok) Serial.println("voice: connecttoFS failed");
  }
}

// ---------------------------------------------------------------------------
// JSON escape (defensive for quotes / newlines in Daemon's replies)
// ---------------------------------------------------------------------------
static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   if ((uint8_t)c >= 0x20) out += c;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Blocking fetch: POST → Piper (LAN) → WAV → LittleFS slot.
// No TLS, no cloud round-trip. Typical wall-clock is 150–400 ms on the
// iPhone hotspot; synthesis cost is zero.
// ---------------------------------------------------------------------------
static bool fetchWavFromPiper(const String &text, const char *outPath) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(PIPER_HOST) == 0)       return false;

  // Voice is Critical priority. Piper is cheap, but other TLS-hungry
  // tasks still shouldn't pile up on top of us.
  NetGate gate("voice", NetGate::Priority::Critical);
  if (!gate.ok()) return false;

  if (!s_piperClient) {
    s_piperClient = new WiFiClient();
    s_piperClient->setNoDelay(true);
  }

  String url = String("http://") + PIPER_HOST + ":" + String(PIPER_PORT) + "/";

  String body = "{\"text\":\"";
  body += jsonEscape(text);
  body += "\"}";

  s_piperHttp.setReuse(true);
  // Piper takes ~200 ms to synthesize short text. 4 s is a generous ceiling
  // that still fails fast enough for the fallback to kick in before the
  // user gives up on the reply.
  s_piperHttp.setTimeout(4000);
  if (!s_piperHttp.begin(*s_piperClient, url)) {
    Serial.println("voice: piper http.begin failed");
    return false;
  }
  s_piperHttp.addHeader("Content-Type", "application/json");
  s_piperHttp.addHeader("Accept",       "audio/wav");
  s_piperHttp.addHeader("Connection",   "keep-alive");

  uint32_t t0 = millis();
  int code = s_piperHttp.POST(body);
  uint32_t tHdr = millis();
  if (code != 200) {
    Serial.printf("voice: piper HTTP %d\n", code);
    s_piperHttp.end();
    s_piperClient->stop();
    return false;
  }

  File f = LittleFS.open(outPath, FILE_WRITE);
  if (!f) {
    Serial.println("voice: LittleFS open failed (piper)");
    s_piperHttp.end();
    return false;
  }
  int written = s_piperHttp.writeToStream(&f);
  f.close();
  s_piperHttp.end();
  if (written <= 0) {
    Serial.printf("voice: piper writeToStream failed (%d)\n", written);
    s_piperClient->stop();
    return false;
  }
  Serial.printf("voice: piper %d bytes WAV in %lu ms (hdr %lu ms) -> %s\n",
                written,
                (unsigned long)(millis() - t0),
                (unsigned long)(tHdr - t0),
                outPath);
  return true;
}

// ---------------------------------------------------------------------------
// Blocking fetch: POST → ElevenLabs → MP3 → LittleFS slot
// ---------------------------------------------------------------------------
static bool fetchMp3FromElevenLabs(const String &text, const char *outPath) {
  if (String(ELEVENLABS_API_KEY).startsWith("PASTE-") ||
      strlen(ELEVENLABS_API_KEY) < 10) {
    Serial.println("voice: no ElevenLabs key configured");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("voice: no wifi");
    return false;
  }

  // ElevenLabs query string, tuned for minimum first-audio latency on
  // ESP32:
  //   mp3_22050_32              ~1/4 the bytes of mp3_44100_128, saves
  //                             500–1500 ms of TLS download per utterance
  //                             with imperceptible quality loss on a
  //                             tiny speaker.
  //   optimize_streaming_latency=4   Max-aggression server-side latency
  //                             mode. Levels 0–4; 4 trades a hair of
  //                             quality for the fastest TTFB ElevenLabs
  //                             offers on the REST endpoint.
  String url = "https://api.elevenlabs.io/v1/text-to-speech/";
  url += ELEVENLABS_VOICE_ID;
  url += "/stream?output_format=mp3_22050_32&optimize_streaming_latency=4";

  String body = "{\"text\":\"";
  body += jsonEscape(text);
  body += "\",\"model_id\":\"";
  body += ELEVENLABS_MODEL;
  body += "\",\"voice_settings\":{\"stability\":0.45,\"similarity_boost\":0.8,"
          "\"style\":0.0,\"use_speaker_boost\":true}}";

  // Voice is Critical priority — user is waiting to hear the reply, so
  // we're willing to wait up to the default 10 s for a slot and heap.
  NetGate gate("voice", NetGate::Priority::Critical);
  if (!gate.ok()) {
    Serial.println("voice: netgate refused — deferring TTS fetch");
    return false;
  }

  // Lazily create the reused TLS client on first use. Keeping it alive
  // across calls lets HTTPClient reuse the TCP+TLS session (see
  // setReuse(true) below) so subsequent utterances skip the ~800 ms
  // handshake entirely.
  if (!s_httpsClient) {
    s_httpsClient = new WiFiClientSecure();
    s_httpsClient->setInsecure();
    // Disable Nagle so small POST bodies (~1 KB JSON) go out immediately
    // instead of waiting up to 40 ms for a coalesce window.
    s_httpsClient->setNoDelay(true);
  }

  s_http.setReuse(true);
  s_http.setTimeout(10000);
  if (!s_http.begin(*s_httpsClient, url)) {
    Serial.println("voice: http.begin failed");
    return false;
  }
  s_http.addHeader("xi-api-key",   ELEVENLABS_API_KEY);
  s_http.addHeader("Content-Type", "application/json");
  s_http.addHeader("Accept",       "audio/mpeg");
  s_http.addHeader("Connection",   "keep-alive");

  uint32_t t0 = millis();
  int code = s_http.POST(body);
  uint32_t tHdr = millis();
  if (code != 200) {
    String err = s_http.getString();
    Serial.printf("voice: HTTP %d from ElevenLabs: %s\n", code, err.c_str());
    s_http.end();
    // A non-200 may have left the connection in a weird state. Force a
    // full reconnect on the next call so we don't thrash.
    s_httpsClient->stop();
    return false;
  }

  File f = LittleFS.open(outPath, FILE_WRITE);
  if (!f) {
    Serial.println("voice: LittleFS open failed");
    s_http.end();
    return false;
  }
  int written = s_http.writeToStream(&f);
  f.close();
  s_http.end();
  if (written <= 0) {
    Serial.printf("voice: writeToStream failed (%d)\n", written);
    // Dead socket — drop it so keep-alive doesn't try to reuse it.
    s_httpsClient->stop();
    return false;
  }
  Serial.printf("voice: %d bytes MP3 in %lu ms (hdr %lu ms) -> %s\n",
                written,
                (unsigned long)(millis() - t0),
                (unsigned long)(tHdr - t0),
                outPath);
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool voiceBegin() {
  Serial.println("voice: direct-I2S hardware probe (you should hear 3 beeps)");
  if (beepInstall()) {
    beepTone(660,  90, 12000);
    beepTone(990,  90, 12000);
    beepTone(1320, 140, 12000);
    delay(150);
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
  } else {
    Serial.println("voice: direct I2S install FAILED");
  }

  if (!LittleFS.begin(true)) {
    Serial.println("voice: LittleFS mount failed");
    return false;
  }

  s_audio = new Audio(/*internalDAC=*/false);
  s_audio->setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  s_audio->setVolume(21);
  s_audio->setTone(6, 6, 6);

  BaseType_t ok = xTaskCreatePinnedToCore(
      audioTaskEntry, "audio", 8192, nullptr, 19, &s_audioTask, 0);
  if (ok != pdPASS) {
    Serial.println("voice: audio task create FAILED");
    return false;
  }

  // A small queue + dedicated task so voiceSpeak() can be non-blocking.
  // Keeping the main loop responsive during the ~2-4 s HTTPS fetch is
  // the single biggest UX improvement for long replies.
  s_speakQueue = xQueueCreate(2, sizeof(SpeakJob));
  if (!s_speakQueue) {
    Serial.println("voice: speak queue alloc FAILED");
    return false;
  }
  // 12 KB stack — we now hold a plaintext Piper client + a TLS
  // ElevenLabs client concurrently, plus some String allocations for
  // path building. 10 KB was tight.
  if (xTaskCreatePinnedToCore(fetchTaskEntry, "voice-fetch", 12288,
                              nullptr, 3, &s_fetchTask, 1) != pdPASS) {
    Serial.println("voice: fetch task create FAILED");
    return false;
  }

  s_ready = true;
  Serial.printf("voice: ready (piper=%s:%d, elevenlabs fallback)\n",
                PIPER_HOST, PIPER_PORT);
  return true;
}

void voiceLoop() {
  if (s_playing && s_audio && !s_audio->isRunning()) s_playing = false;
}

bool voiceSpeak(const String &text) {
  if (!s_ready || !s_audio || !s_speakQueue) return false;
  if (text.length() == 0) return false;

  SpeakJob job;
  memset(&job, 0, sizeof(job));
  size_t n = text.length();
  if (n >= sizeof(job.text)) n = sizeof(job.text) - 1;
  memcpy(job.text, text.c_str(), n);
  job.text[n] = '\0';

  // If the fetch queue already has a job in flight (or queued), drop the
  // oldest so the newest utterance wins — matches the old "stopSong +
  // start new" behaviour without blocking the caller.
  if (uxQueueSpacesAvailable(s_speakQueue) == 0) {
    SpeakJob drop;
    xQueueReceive(s_speakQueue, &drop, 0);
  }
  // Flip busy BEFORE the send so any background task checking
  // voiceIsBusy() right after this call sees the pending work and
  // doesn't race us into mbedTLS OOM.
  s_fetching = true;
  bool queued = (xQueueSend(s_speakQueue, &job, 0) == pdTRUE);
  if (!queued) s_fetching = false;
  return queued;
}

bool voiceIsSpeaking() { return s_playing; }
bool voiceIsBusy()     { return s_fetching || s_playing; }

void voiceStop() {
  if (s_audio && s_audio->isRunning()) s_audio->stopSong();
  s_playing = false;
}

void voiceDiagnose() { /* no-op now */ }

void voiceSetVolume(uint8_t v) {
  if (v > 21) v = 21;
  if (s_audio) s_audio->setVolume(v);
}

void audio_info(const char *info)    { Serial.print("[audio] ");    Serial.println(info); }
void audio_id3data(const char *info) { Serial.print("[audio id3] "); Serial.println(info); }
void audio_bitrate(const char *info) { Serial.print("[audio br] ");  Serial.println(info); }
void audio_eof_mp3(const char *f)    { (void)f; Serial.println("[audio] eof mp3"); s_playing = false; }
void audio_eof_stream(const char *f) { (void)f; Serial.println("[audio] eof stream"); s_playing = false; }

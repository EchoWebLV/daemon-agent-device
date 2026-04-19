#include "voice.h"
#include "secrets.h"

#include <Audio.h>
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

// ElevenLabs response cached into LittleFS and played back through the
// Audio library's proven connecttoFS path. This is a known-working flow;
// the streaming-direct-to-decoder patch had a premature EOF bug in the
// library's chunked reader that cut audio off after ~6 KB.
static constexpr const char *TTS_FS_PATH = "/tts.mp3";

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
// Fetch task — runs the blocking ElevenLabs HTTPS POST + LittleFS write
// off the main Arduino loop so the UI keeps ticking while Daemon fetches
// his reply audio. When the MP3 lands in /tts.mp3 we kick off playback.
// ---------------------------------------------------------------------------
static bool fetchMp3ToFs(const String &text);   // defined further down

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

    // Stop any currently playing utterance AND wait for the Audio library
    // to actually release the LittleFS handle on /tts.mp3 before we
    // reopen it for writing. Without this wait, LittleFS asserts with
    // `lfs_mlist_isopen` on the next lfs_file_write because the old read
    // handle is still on its open-files list.
    if (s_audio->isRunning()) {
      s_audio->stopSong();
      uint32_t deadline = millis() + 500;
      while (s_audio->isRunning() && millis() < deadline) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      // Tiny extra grace so the audio task's current loop() iteration
      // definitely finishes its File::close() path.
      vTaskDelay(30 / portTICK_PERIOD_MS);
    }

    if (!fetchMp3ToFs(text)) { s_fetching = false; continue; }

    bool ok = s_audio->connecttoFS(LittleFS, TTS_FS_PATH);
    s_playing  = ok;
    s_fetching = false;
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
// Blocking fetch: POST → ElevenLabs → MP3 → LittleFS
// ---------------------------------------------------------------------------
static bool fetchMp3ToFs(const String &text) {
  if (String(ELEVENLABS_API_KEY).startsWith("PASTE-") ||
      strlen(ELEVENLABS_API_KEY) < 10) {
    Serial.println("voice: no ElevenLabs key configured");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("voice: no wifi");
    return false;
  }

  String url = "https://api.elevenlabs.io/v1/text-to-speech/";
  url += ELEVENLABS_VOICE_ID;
  url += "/stream?output_format=mp3_44100_128";

  String body = "{\"text\":\"";
  body += jsonEscape(text);
  body += "\",\"model_id\":\"";
  body += ELEVENLABS_MODEL;
  body += "\",\"voice_settings\":{\"stability\":0.45,\"similarity_boost\":0.8,"
          "\"style\":0.3,\"use_speaker_boost\":true}}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(25000);
  if (!http.begin(client, url)) {
    Serial.println("voice: http.begin failed");
    return false;
  }
  http.addHeader("xi-api-key",   ELEVENLABS_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept",       "audio/mpeg");

  uint32_t t0 = millis();
  int code = http.POST(body);
  if (code != 200) {
    String err = http.getString();
    Serial.printf("voice: HTTP %d from ElevenLabs: %s\n", code, err.c_str());
    http.end();
    return false;
  }

  File f = LittleFS.open(TTS_FS_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("voice: LittleFS open failed");
    http.end();
    return false;
  }
  int written = http.writeToStream(&f);
  f.close();
  http.end();
  if (written <= 0) {
    Serial.printf("voice: writeToStream failed (%d)\n", written);
    return false;
  }
  Serial.printf("voice: %d bytes MP3 in %lu ms\n",
                written, (unsigned long)(millis() - t0));
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
  if (xTaskCreatePinnedToCore(fetchTaskEntry, "voice-fetch", 10240,
                              nullptr, 3, &s_fetchTask, 1) != pdPASS) {
    Serial.println("voice: fetch task create FAILED");
    return false;
  }

  s_ready = true;
  Serial.println("voice: LittleFS+MP3 path ready (async fetch on)");
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

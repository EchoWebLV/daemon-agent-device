#include "voice.h"
#include "secrets.h"
#include "testharness.h"   // testHarnessInTestMode() — mutes audio during tests

#include <Audio.h>
#include <WiFi.h>
#include <LittleFS.h>      // still mounted for other callers (logs, tts cache)
#include <driver/i2s.h>

// ---------------------------------------------------------------------------
// I2S pins (PCM5101 on the Waveshare 2.8")
// ---------------------------------------------------------------------------
static constexpr int I2S_BCLK = 48;
static constexpr int I2S_LRC  = 38;
static constexpr int I2S_DOUT = 47;

// Streaming path via the library's injected connecttoElevenlabs() method
// (see patches/audio_elevenlabs_patch.py). The HTTPS POST opens, and the
// MP3 response body is piped straight into Helix → I2S as the bytes
// arrive — no LittleFS round-trip, no "wait for full download before
// first sample" dead air.
static Audio       *s_audio      = nullptr;
static TaskHandle_t s_audioTask  = nullptr;
static volatile bool s_ready     = false;
static volatile bool s_playing   = false;
// Target "steady-state" volume. voiceSpeak ramps from 0 up to this to hide
// the I2S/decoder start-up click; voiceSetVolume updates both it and the
// live audio gain.
static volatile uint8_t s_targetVol = 21;

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
  s_ready = true;
  Serial.println("voice: LittleFS+MP3 path ready");
  return true;
}

void voiceLoop() {
  if (s_playing && s_audio && !s_audio->isRunning()) s_playing = false;
}

bool voiceSpeak(const String &text) {
  if (!s_ready || !s_audio) return false;
  if (text.length() == 0) return false;
  // Skip TTS during smoke tests: the boot greeting otherwise fetches an
  // MP3 and streams it *after* loop() starts (well after the host has
  // already flipped the device into test mode), and the Audio library's
  // core-0 prints scramble the single-line TEST replies.
  if (testHarnessInTestMode()) return false;

  if (String(ELEVENLABS_API_KEY).startsWith("PASTE-") ||
      strlen(ELEVENLABS_API_KEY) < 10) {
    Serial.println("voice: no ElevenLabs key configured");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("voice: no wifi");
    return false;
  }

  if (s_audio->isRunning()) s_audio->stopSong();

  // Start playback at volume 0 to hide the audible click produced by the
  // I2S/DAC state change when the decoder kicks in. The Audio library's
  // setVolume is a cheap gain field — the audio task on core 0 picks up
  // the new value on the next DMA chunk, so we can ramp from the main
  // thread with small blocking delays.
  //
  // With the streaming path, connecttoElevenlabs() returns as soon as the
  // POST has been written and the server's response headers are being
  // read. The first decoded PCM frame typically lands 150–300 ms later,
  // so this ramp is running during that "almost-but-not-quite-speaking"
  // window and hides both the DAC pop and the initial decoder jitter.
  const uint8_t target = s_targetVol;
  s_audio->setVolume(0);

  uint32_t t0 = millis();
  bool ok = s_audio->connecttoElevenlabs(
      ELEVENLABS_VOICE_ID,
      ELEVENLABS_API_KEY,
      text.c_str(),
      ELEVENLABS_MODEL);   // usually "eleven_flash_v2_5"; see secrets.h
  Serial.printf("voice: connecttoElevenlabs -> %s in %lu ms\n",
                ok ? "ok" : "FAIL", (unsigned long)(millis() - t0));

  if (ok) {
    // ~48 ms fade-in — long enough to smear the step across many DMA
    // buffers, short enough that speech still feels instant.
    for (int v = 0; v <= (int)target; v += 3) {
      s_audio->setVolume((uint8_t)(v > target ? target : v));
      delay(6);
    }
    s_audio->setVolume(target);
  } else {
    s_audio->setVolume(target);
    Serial.println("voice: ElevenLabs stream start failed");
  }
  s_playing = ok;
  return ok;
}

bool voiceIsSpeaking() { return s_playing; }

void voiceStop() {
  if (s_audio && s_audio->isRunning()) s_audio->stopSong();
  s_playing = false;
}

void voiceDiagnose() { /* no-op now */ }

void voiceSetVolume(uint8_t v) {
  if (v > 21) v = 21;
  s_targetVol = v;
  if (s_audio) s_audio->setVolume(v);
}

// The Audio library fires these callbacks from a different FreeRTOS task,
// so their Serial.print calls race the main loop's prints and splice
// mid-line (e.g. "[audio] Ch[audio] Sa"). Harmless in normal operation,
// but it scrambles the single-line TEST protocol — so we suppress audio
// logging while a test session is active. (testharness.h is included
// at the top of this file so voiceSpeak() can also gate on test mode.)
static inline bool audioLogOk() { return !testHarnessInTestMode(); }
void audio_info(const char *info)    { if (audioLogOk()) { Serial.print("[audio] ");    Serial.println(info); } }
void audio_id3data(const char *info) { if (audioLogOk()) { Serial.print("[audio id3] "); Serial.println(info); } }
void audio_bitrate(const char *info) { if (audioLogOk()) { Serial.print("[audio br] ");  Serial.println(info); } }
void audio_eof_mp3(const char *f)    { (void)f; if (audioLogOk()) Serial.println("[audio] eof mp3"); s_playing = false; }
void audio_eof_stream(const char *f) { (void)f; if (audioLogOk()) Serial.println("[audio] eof stream"); s_playing = false; }

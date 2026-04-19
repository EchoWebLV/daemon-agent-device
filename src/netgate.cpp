#include "netgate.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
// Max simultaneous TLS sessions. mbedTLS on the ESP32-S3 Arduino core
// uses ~30 KB of internal DRAM per active context (handshake buffers +
// per-connection state). Two at once is safe; three peaks close to the
// fragmentation cliff. Observed crashes under load were almost always
// 3+ contexts competing for internal heap.
static constexpr int MAX_CONCURRENT_TLS = 2;

// Minimum free internal DRAM we insist on before letting a new TLS
// session start. ~60 KB leaves enough contiguous room for an mbedTLS
// handshake plus the HTTPClient response buffer. Below this we make
// the caller wait rather than risk a malloc failure inside the TLS
// stack (which asserts/aborts on the ESP32).
static constexpr uint32_t MIN_FREE_INTERNAL_DRAM = 60 * 1024;

// Low-priority callers give up earlier so they don't pile up while
// Critical callers (voice, LLM) are holding both slots.
static constexpr uint32_t LOW_PRIO_SLACK_MS = 2000;

// ---------------------------------------------------------------------------
// Singleton gate state
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_slots = nullptr;   // counting semaphore
static volatile int      s_inFlight = 0;       // best-effort counter for logs

static void ensureGate() {
  if (!s_slots) {
    // Counting semaphore pre-initialised to MAX so the first few Takes
    // succeed immediately. Subsequent ones block until a Give.
    s_slots = xSemaphoreCreateCounting(MAX_CONCURRENT_TLS,
                                       MAX_CONCURRENT_TLS);
  }
}

static bool heapOk() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= MIN_FREE_INTERNAL_DRAM;
}

// ---------------------------------------------------------------------------
// NetGate RAII
// ---------------------------------------------------------------------------
NetGate::NetGate(const char *tag, Priority prio, uint32_t waitTimeoutMs)
    : acquired_(false), tag_(tag ? tag : "?"), prio_(prio) {
  ensureGate();
  if (!s_slots) return;

  // Low-priority callers shouldn't block for long — better to skip this
  // tick and come back in a few seconds.
  uint32_t timeoutMs = waitTimeoutMs;
  if (prio == Priority::Low && timeoutMs > LOW_PRIO_SLACK_MS) {
    timeoutMs = LOW_PRIO_SLACK_MS;
  }

  // First acquire a slot.
  if (xSemaphoreTake(s_slots, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    Serial.printf("netgate[%s]: slot timeout (prio=%u inflight=%d)\n",
                  tag_, (unsigned)prio_, s_inFlight);
    return;
  }

  // Then wait for DRAM. Poll every 100 ms up to the same timeout so a
  // transient allocation spike doesn't force us to give up immediately.
  uint32_t deadline = millis() + timeoutMs;
  while (!heapOk()) {
    if (millis() > deadline) {
      Serial.printf("netgate[%s]: dram floor (free=%u need=%u prio=%u)\n",
                    tag_,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)MIN_FREE_INTERNAL_DRAM,
                    (unsigned)prio_);
      xSemaphoreGive(s_slots);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  acquired_ = true;
  s_inFlight++;
}

NetGate::~NetGate() {
  if (acquired_ && s_slots) {
    s_inFlight--;
    xSemaphoreGive(s_slots);
  }
}

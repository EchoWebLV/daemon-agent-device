// ============================================================================
//  netgate — co-ordinates concurrent HTTPS/TLS sessions across tasks.
//
//  Every place that constructs a `WiFiClientSecure` should declare a
//  `NetGate` scope-guard first:
//
//      NetGate gate("price", NetGate::Priority::Low);
//      if (!gate.ok()) return false;
//      WiFiClientSecure client;
//      ...
//
//  The guard does two things:
//    1. Counts the number of active TLS sessions and blocks new ones when
//       the global cap is already in flight. ESP32-S3 internal DRAM only
//       has room for ~2 simultaneous mbedTLS contexts on top of the
//       sprite + task stacks + WebServer buffers; a third one reliably
//       triggers a mbedtls malloc failure → abort → reset.
//    2. Refuses to open new sessions when free internal DRAM is below a
//       safety threshold (~60 KB), so we defer rather than crash.
//
//  The previous policy was "voiceIsBusy + 70 KB floor" hand-rolled in
//  memory.cpp. That worked for the Arweave leg but left every other TLS
//  callsite unprotected, which is the suspected cause of the spontaneous
//  resets reported in the field.
// ============================================================================
#pragma once
#include <Arduino.h>

class NetGate {
public:
  enum class Priority : uint8_t {
    Critical = 0,  // voice, LLM — user is waiting, don't reject if possible
    Normal   = 1,  // xpost, memory, arweave
    Low      = 2,  // price, wallet refresh — OK to skip this cycle
  };

  explicit NetGate(const char *tag,
                   Priority prio = Priority::Normal,
                   uint32_t waitTimeoutMs = 10000);
  ~NetGate();

  bool ok() const { return acquired_; }

private:
  bool acquired_;
  const char *tag_;
  Priority    prio_;
};

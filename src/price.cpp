#include "price.h"
#include "netgate.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static double s_priceUsd = 0.0;

bool priceBegin() {
  s_priceUsd = 0.0;
  return true;
}

void priceRefresh() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Price ticker is Low priority — if the gate refuses (heap tight or
  // both TLS slots busy), we just skip this cycle and retry in 30 s.
  NetGate gate("price", NetGate::Priority::Low);
  if (!gate.ok()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client,
      "https://api.coingecko.com/api/v3/simple/price?ids=solana&vs_currencies=usd")) {
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("price: HTTP %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;
  double p = doc["solana"]["usd"].as<double>();
  if (p > 0) {
    s_priceUsd = p;
    Serial.printf("price: SOL $%.2f\n", s_priceUsd);
  }
}

double priceSOLUSD() { return s_priceUsd; }

// --- Background task --------------------------------------------------
static TaskHandle_t      s_task    = nullptr;
static SemaphoreHandle_t s_trigger = nullptr;

static void priceTaskEntry(void *) {
  for (;;) {
    if (xSemaphoreTake(s_trigger, portMAX_DELAY) == pdTRUE) {
      priceRefresh();
    }
  }
}

static void ensurePriceTask() {
  if (s_task) return;
  s_trigger = xSemaphoreCreateBinary();
  // 8 KB — TLS handshake + HTTPClient + ArduinoJson easily use >4 KB.
  // A smaller stack here causes the canary watchpoint to trigger.
  xTaskCreatePinnedToCore(priceTaskEntry, "price", 8192, nullptr,
                          1, &s_task, 1);
}

void priceRequestRefresh() {
  ensurePriceTask();
  if (s_trigger) xSemaphoreGive(s_trigger);
}

String priceDisplayString() {
  if (s_priceUsd <= 0) return "SOL --";
  char buf[24];
  snprintf(buf, sizeof(buf), "SOL $%.2f", s_priceUsd);
  return String(buf);
}

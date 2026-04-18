#include "price.h"

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

String priceDisplayString() {
  if (s_priceUsd <= 0) return "SOL --";
  char buf[24];
  snprintf(buf, sizeof(buf), "SOL $%.2f", s_priceUsd);
  return String(buf);
}

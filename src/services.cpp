#include "services.h"
#include "devcfg.h"

#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Built-in catalog — 1:1 with the extension's src/lib/x402-services.ts.
// Kept deliberately compact; the HTML served to the browser also has this
// list hardcoded (for the Services tab UI) so both sides stay in sync.
// ---------------------------------------------------------------------------
static Service svc(const char *id, const char *name, const char *category,
                   const char *baseUrl, const char *priceRange,
                   const char *description,
                   std::vector<ServiceEndpoint> endpoints) {
  Service s;
  s.id = id; s.name = name; s.category = category;
  s.baseUrl = baseUrl; s.priceRange = priceRange;
  s.description = description;
  s.endpoints = std::move(endpoints);
  return s;
}
static ServiceEndpoint ep(const char *id, const char *method, const char *path,
                          const char *description,
                          std::vector<ServiceParam> params = {}) {
  ServiceEndpoint e;
  e.id = id; e.method = method; e.path = path; e.description = description;
  e.params = std::move(params);
  return e;
}
static ServiceParam par(const char *name, bool required) {
  ServiceParam p; p.name = name; p.required = required; return p;
}

static std::vector<Service> buildBuiltin() {
  return {
    svc("solsignal", "SolSignal", "security",
        "https://solsignal-api.onrender.com", "$0.01",
        "Solana token safety scanner — one SAFE/AVOID verdict from DexScreener, RugCheck, GoPlus & Jupiter simulation.",
        { ep("scan", "GET", "/api/scan",
             "Scan a Solana token for rug-pull risk. Pass the token mint.",
             { par("token", true) }) }),

    svc("sentinel", "Sentinel", "security",
        "https://sentinel-awms.onrender.com", "$0.005 - $0.025",
        "Trust verification — protocol trust scoring, token safety, OFAC screening.",
        { ep("trust-score", "GET", "/api/trust-score",
             "Trust/safety score for an address or protocol.",
             { par("address", true) }),
          ep("ofac-check", "GET", "/api/ofac-check",
             "Screen an address against the OFAC sanctions list.",
             { par("address", true) }) }),

    svc("cybera", "CYBERA Compliance", "security",
        "https://compliance-api-ruddy.vercel.app", "$0.01",
        "VASP / entity identification across 29 chains, risk scoring, mixer screening.",
        { ep("identify", "GET", "/api/identify",
             "Identify the VASP or entity behind a blockchain address.",
             { par("address", true), par("chain", false) }) }),

    svc("defi-signal", "DeFi Signal", "defi",
        "https://defi-signal-agent-production.up.railway.app", "$0.01 - $0.10",
        "Pool risk scores and Solana whale alerts (>$100K), Dune-enriched on-chain intel.",
        { ep("pool-risk", "GET", "/api/pool-risk",
             "Get risk score (0-10) for a DeFi pool.",
             { par("pool", true) }),
          ep("whale-alerts", "GET", "/api/whale-alerts",
             "Recent Solana whale movements above $100K.",
             {}) }),

    svc("deepblue", "DeepBlue Trading", "defi",
        "https://api.deepbluebase.xyz", "$0.01 - $0.05",
        "AI crypto intelligence — trading signals, prediction analytics, whale tracking.",
        { ep("signals", "GET", "/api/signals",
             "AI-generated trading signals for a token symbol.",
             { par("token", false) }),
          ep("whales", "GET", "/api/whales",
             "Whale wallet movements and large transactions.",
             {}) }),

    svc("moonmaker", "MoonMaker", "defi",
        "https://api.moonmaker.cc", "$0.02 - $0.10",
        "AI-native crypto data — market context, DeFi regime detection, DEX alpha.",
        { ep("market-context", "GET", "/api/market-context",
             "Current market regime + directional signals.",
             {}),
          ep("dex-alpha", "GET", "/api/dex-alpha",
             "DEX trading alpha signals and opportunities.",
             {}) }),

    svc("kerdos", "Kerdos Market Intel", "defi",
        "https://nonvisceral-eloisa-mousily.ngrok-free.dev", "$0.01 - $0.05",
        "Sentiment scoring, regime direction, funding rates, liquidation risk.",
        { ep("sentiment", "GET", "/api/sentiment",
             "Crypto market sentiment + fear/greed analysis.",
             {}),
          ep("funding-rates", "GET", "/api/funding-rates",
             "Current funding rates across major exchanges.",
             {}) }),

    svc("xquik", "Xquik", "data",
        "https://xquik.com", "$0.001 - $0.01",
        "Real-time X (Twitter) data API — tweet search, user profiles, trends.",
        { ep("search", "GET", "/api/search",
             "Search recent tweets. Useful for crypto sentiment and news.",
             { par("q", true), par("limit", false) }),
          ep("trends", "GET", "/api/trends",
             "Current trending topics on X.",
             {}) }),

    svc("agentfeed", "AgentFeed", "data",
        "https://agentfeed.io", "$0.001 - $0.02",
        "Structured data feeds — market, news, social signals.",
        { ep("feed", "GET", "/api/feed",
             "Fetch a structured data feed (market, news, or social).",
             { par("type", true), par("token", false) }) }),

    svc("gpu-bridge", "GPU-Bridge", "ai",
        "https://gpubridge.xyz", "$0.00002 - $0.001",
        "30-service GPU inference — LLM, image gen, embeddings, STT, TTS.",
        { ep("inference", "POST", "/api/inference",
             "Run text generation on a GPU model.",
             { par("model", true), par("prompt", true), par("max_tokens", false) }) }),

    svc("x402engine", "x402engine", "infra",
        "https://x402engine.app", "$0.001 - $0.50",
        "74 endpoints — 44 LLMs, image/video gen, crypto data, web search, code exec.",
        { ep("web-search", "GET", "/api/search",
             "Search the web and get structured results. Use for real-time info.",
             { par("q", true) }) }),
  };
}

// Cached so we don't rebuild on every aiAsk().
static std::vector<Service> s_builtin;
static bool                 s_builtinReady = false;

static void ensureBuiltin() {
  if (!s_builtinReady) {
    s_builtin = buildBuiltin();
    s_builtinReady = true;
  }
}

// ---------------------------------------------------------------------------
// Deserialize a custom service JSON object (as produced by the browser's
// skill parser + POSTed via /config) into our C++ struct.
// ---------------------------------------------------------------------------
static Service deserCustom(JsonObjectConst obj) {
  Service s;
  s.custom      = true;
  s.id          = obj["id"].as<const char*>()       ? String(obj["id"].as<const char*>())       : String();
  s.name        = obj["name"].as<const char*>()     ? String(obj["name"].as<const char*>())     : String();
  s.description = obj["description"].as<const char*>() ? String(obj["description"].as<const char*>()) : String();
  s.category    = obj["category"].as<const char*>() ? String(obj["category"].as<const char*>()) : String("data");
  s.baseUrl     = obj["baseUrl"].as<const char*>()  ? String(obj["baseUrl"].as<const char*>())  : String();
  s.priceRange  = obj["priceRange"].as<const char*>() ? String(obj["priceRange"].as<const char*>()) : String();

  JsonArrayConst eps = obj["endpoints"].as<JsonArrayConst>();
  if (!eps.isNull()) {
    for (JsonObjectConst e : eps) {
      ServiceEndpoint se;
      se.id          = e["id"].as<const char*>()          ? String(e["id"].as<const char*>())          : String("call");
      se.method      = e["method"].as<const char*>()      ? String(e["method"].as<const char*>())      : String("POST");
      se.path        = e["path"].as<const char*>()        ? String(e["path"].as<const char*>())        : String("/");
      const char *td = e["toolDescription"].as<const char*>();
      se.description = td ? String(td) : s.description;
      JsonArrayConst ps = e["params"].as<JsonArrayConst>();
      if (!ps.isNull()) {
        for (JsonObjectConst p : ps) {
          ServiceParam sp;
          sp.name     = p["name"].as<const char*>() ? String(p["name"].as<const char*>()) : String();
          sp.required = p["required"] | false;
          if (sp.name.length() > 0) se.params.push_back(sp);
        }
      }
      s.endpoints.push_back(se);
    }
  }
  return s;
}

std::vector<Service> servicesAll() {
  ensureBuiltin();
  std::vector<Service> out = s_builtin;

  // Merge custom services from NVS.
  String customJson = devcfgCustomServices();
  if (customJson.length() > 0 && customJson != "[]") {
    JsonDocument doc;
    if (deserializeJson(doc, customJson, DeserializationOption::NestingLimit(16)) == DeserializationError::Ok) {
      JsonArrayConst arr = doc.as<JsonArrayConst>();
      if (!arr.isNull()) {
        for (JsonObjectConst o : arr) {
          if (o.isNull()) continue;
          Service s = deserCustom(o);
          if (s.id.length() > 0 && s.baseUrl.length() > 0) out.push_back(s);
        }
      }
    }
  }
  return out;
}

std::vector<Service> servicesEnabled() {
  std::vector<Service> all = servicesAll();

  String enabledJson = devcfgServicesEnabled();
  JsonDocument doc;
  if (deserializeJson(doc, enabledJson)) return {};
  JsonArrayConst ids = doc.as<JsonArrayConst>();
  if (ids.isNull()) return {};

  std::vector<String> on;
  for (JsonVariantConst v : ids) {
    const char *id = v.as<const char*>();
    if (id && *id) on.emplace_back(id);
  }

  std::vector<Service> out;
  for (const Service &s : all) {
    for (const String &id : on) {
      if (s.id == id) { out.push_back(s); break; }
    }
  }
  return out;
}

const Service *serviceById(const std::vector<Service> &list, const String &id) {
  for (const Service &s : list) if (s.id == id) return &s;
  return nullptr;
}

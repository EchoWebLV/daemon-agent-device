#include "services.h"
#include "devcfg.h"
#include "x402.h"

#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Mirror of the BUILTIN array in server.cpp (the one the settings UI renders).
// Must be kept in sync by hand — this is the single place the firmware reads
// from when the model asks to invoke a tool. Keep descriptions concise; every
// enabled service lands in every chat request the model sees.
// ---------------------------------------------------------------------------
static const char BUILTIN_SERVICES_JSON[] = R"JSON([
{"id":"solsignal","name":"SolSignal","category":"security","baseUrl":"https://solsignal-api.onrender.com","description":"Solana token safety scanner (DexScreener, RugCheck, GoPlus, Jupiter simulation) returning a SAFE/AVOID verdict.","endpoints":[{"id":"scan","method":"GET","path":"/api/scan","params":[{"name":"token","required":true,"description":"Solana token mint address to scan"}]}]},
{"id":"sentinel","name":"Sentinel","category":"security","baseUrl":"https://sentinel-awms.onrender.com","description":"Trust verification — protocol trust scoring, token safety, DeFi risk, OFAC screening.","endpoints":[{"id":"trust-score","method":"GET","path":"/api/trust-score","params":[]},{"id":"ofac-check","method":"GET","path":"/api/ofac-check","params":[]}]},
{"id":"cybera","name":"CYBERA Compliance","category":"security","baseUrl":"https://compliance-api-ruddy.vercel.app","description":"VASP address identification across 29 chains, risk scoring, sanctions & mixer screening.","endpoints":[{"id":"identify","method":"GET","path":"/api/identify","params":[]}]},
{"id":"defi-signal","name":"DeFi Signal","category":"defi","baseUrl":"https://defi-signal-agent-production.up.railway.app","description":"New pool risk scoring (0-10), Solana whale alerts (>$100K), and Dune-enriched on-chain intel.","endpoints":[{"id":"pool-risk","method":"GET","path":"/api/pool-risk","params":[]},{"id":"whale-alerts","method":"GET","path":"/api/whale-alerts","params":[]}]},
{"id":"deepblue","name":"DeepBlue Trading","category":"defi","baseUrl":"https://api.deepbluebase.xyz","description":"AI crypto intelligence — live signals, prediction analytics, whale tracking.","endpoints":[{"id":"signals","method":"GET","path":"/api/signals","params":[]},{"id":"whales","method":"GET","path":"/api/whales","params":[]}]},
{"id":"moonmaker","name":"MoonMaker","category":"defi","baseUrl":"https://api.moonmaker.cc","description":"AI-native crypto data — signals, market context, DeFi regime detection, ETF flows, DEX alpha.","endpoints":[{"id":"market-context","method":"GET","path":"/api/market-context","params":[]},{"id":"dex-alpha","method":"GET","path":"/api/dex-alpha","params":[]}]},
{"id":"kerdos","name":"Kerdos Market Intel","category":"defi","baseUrl":"https://nonvisceral-eloisa-mousily.ngrok-free.dev","description":"Crypto sentiment scoring, BTC/ETH regime direction, funding rates, liquidation risk.","endpoints":[{"id":"sentiment","method":"GET","path":"/api/sentiment","params":[]},{"id":"funding-rates","method":"GET","path":"/api/funding-rates","params":[]}]},
{"id":"xquik","name":"Xquik","category":"data","baseUrl":"https://xquik.com","description":"Real-time X (Twitter) data — tweet search, user profiles, trends.","endpoints":[{"id":"search","method":"GET","path":"/api/search","params":[{"name":"q","required":true,"description":"search query"}]},{"id":"trends","method":"GET","path":"/api/trends","params":[]}]},
{"id":"agentfeed","name":"AgentFeed","category":"data","baseUrl":"https://agentfeed.io","description":"Real-time structured data feeds — market data, news, and social signals.","endpoints":[{"id":"feed","method":"GET","path":"/api/feed","params":[{"name":"type","required":true,"description":"feed type (market, news, social)"}]}]},
{"id":"gpu-bridge","name":"GPU-Bridge","category":"ai","baseUrl":"https://gpubridge.xyz","description":"30-service GPU inference — LLM, image gen, embeddings, STT, TTS, PDF processing.","endpoints":[{"id":"inference","method":"POST","path":"/api/inference","params":[{"name":"model","required":true,"description":"model id"},{"name":"prompt","required":true,"description":"input prompt"}]}]},
{"id":"x402engine","name":"x402engine","category":"infra","baseUrl":"https://x402engine.app","description":"74 endpoints: 44 LLMs, image/video generation, crypto data, web search, code execution, TTS, IPFS.","endpoints":[{"id":"web-search","method":"GET","path":"/api/search","params":[{"name":"q","required":true,"description":"search query"}]}]}
])JSON";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Minimal URL-encoder for query string values. Keeps unreserved characters
// (per RFC 3986) as-is and percent-encodes everything else.
static String urlEncode(const String &v) {
  String out;
  out.reserve(v.length() * 2);
  for (size_t i = 0; i < v.length(); ++i) {
    char c = v[i];
    if ((c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Extract a string-ish representation of a JsonVariant suitable for a URL
// query value. Strings come out unquoted; numbers/bools are stringified.
static String jsonValueToString(JsonVariantConst v) {
  if (v.is<const char *>()) return String((const char *)v);
  if (v.is<bool>()) return v ? "true" : "false";
  if (v.is<float>() || v.is<double>()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", (double)v);
    return String(buf);
  }
  if (v.is<int>() || v.is<long>() || v.is<long long>()) {
    return String((long long)v);
  }
  String s;
  serializeJson(v, s);
  return s;
}

// Parse the combined catalog (BUILTIN + any user-added custom services) into
// `out`. The resulting document holds an array of service records, each with
// id / name / category / baseUrl / description / endpoints[].
static void loadCatalog(JsonDocument &out) {
  JsonArray arr = out.to<JsonArray>();

  JsonDocument builtin;
  if (!deserializeJson(builtin, BUILTIN_SERVICES_JSON)) {
    for (JsonVariantConst s : builtin.as<JsonArrayConst>()) arr.add(s);
  }

  String customJson = devcfgCustomServices();
  customJson.trim();
  if (customJson.length() > 0 && customJson != "[]") {
    JsonDocument custom;
    if (!deserializeJson(custom, customJson)) {
      for (JsonVariantConst s : custom.as<JsonArrayConst>()) arr.add(s);
    }
  }
}

static bool isEnabledId(JsonArrayConst enabled, const char *id) {
  if (!id || !*id || enabled.isNull()) return false;
  for (JsonVariantConst e : enabled) {
    const char *s = e.as<const char *>();
    if (s && strcmp(s, id) == 0) return true;
  }
  return false;
}

static JsonVariantConst findService(JsonArrayConst all, const String &id) {
  for (JsonVariantConst s : all) {
    const char *sid = s["id"] | "";
    if (id == sid) return s;
  }
  return JsonVariantConst();
}

static JsonVariantConst findEndpoint(JsonVariantConst svc, const String &id) {
  JsonArrayConst eps = svc["endpoints"].as<JsonArrayConst>();
  if (eps.isNull()) return JsonVariantConst();
  for (JsonVariantConst e : eps) {
    const char *eid = e["id"] | "";
    if (id == eid) return e;
  }
  return JsonVariantConst();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
String servicesBuildToolsArray() {
  String enabledJson = devcfgServicesEnabled();
  enabledJson.trim();
  if (enabledJson.length() == 0 || enabledJson == "[]") return String();

  JsonDocument enabledDoc;
  if (deserializeJson(enabledDoc, enabledJson)) return String();
  JsonArrayConst enabled = enabledDoc.as<JsonArrayConst>();
  if (enabled.isNull() || enabled.size() == 0) return String();

  JsonDocument catalog;
  loadCatalog(catalog);

  JsonDocument toolsDoc;
  JsonArray tools = toolsDoc.to<JsonArray>();

  for (JsonVariantConst svc : catalog.as<JsonArrayConst>()) {
    const char *sid = svc["id"] | "";
    if (!isEnabledId(enabled, sid)) continue;
    const char *sname = svc["name"] | sid;
    const char *sdesc = svc["description"] | "";
    JsonArrayConst eps = svc["endpoints"].as<JsonArrayConst>();
    if (eps.isNull()) continue;

    for (JsonVariantConst ep : eps) {
      const char *eid    = ep["id"]     | "call";
      const char *method = ep["method"] | "GET";
      const char *path   = ep["path"]   | "/";

      JsonObject tool = tools.add<JsonObject>();
      tool["type"] = "function";
      JsonObject fn = tool["function"].to<JsonObject>();

      String fname = String(sid) + "__" + String(eid);
      fn["name"] = fname;

      String fdesc;
      fdesc.reserve(strlen(sname) + strlen(sdesc) + 48);
      fdesc  = sname;
      fdesc += " — ";
      fdesc += sdesc;
      fdesc += " (";
      fdesc += method;
      fdesc += " ";
      fdesc += path;
      fdesc += ")";
      fn["description"] = fdesc;

      JsonObject params = fn["parameters"].to<JsonObject>();
      params["type"] = "object";
      JsonObject props = params["properties"].to<JsonObject>();
      JsonArray  required = params["required"].to<JsonArray>();

      JsonArrayConst epParams = ep["params"].as<JsonArrayConst>();
      if (!epParams.isNull()) {
        for (JsonVariantConst p : epParams) {
          const char *pname = p["name"] | "";
          if (!*pname) continue;
          const char *pdesc = p["description"] | pname;
          JsonObject prop = props[pname].to<JsonObject>();
          prop["type"] = "string";
          prop["description"] = pdesc;
          if (p["required"] | false) required.add(pname);
        }
      }
    }
  }

  if (tools.size() == 0) return String();
  String out;
  serializeJson(tools, out);
  return out;
}

String servicesDispatchToolCall(const String &name,
                                const String &argsJson,
                                double *outCost) {
  if (outCost) *outCost = 0.0;

  // Tool name format is `<serviceId>__<endpointId>`, but serviceId can itself
  // contain `__` (e.g. `custom_https___registry_frames_ag__api_service_...`
  // is what we generate from arbitrary URLs). So we can't just split on the
  // first `__`. Instead, enumerate known services and pick the one whose id
  // is a `<prefix>__` of the tool name. This matches any service id length
  // and is robust to underscores inside the id.
  JsonDocument catalog;
  loadCatalog(catalog);
  String serviceId;
  String endpointId;
  JsonVariantConst svc;
  for (JsonVariantConst s : catalog.as<JsonArrayConst>()) {
    const char *sid = s["id"] | "";
    if (!*sid) continue;
    String sidStr(sid);
    String probe = sidStr + "__";
    if (name.startsWith(probe) && name.length() > probe.length()) {
      serviceId  = sidStr;
      endpointId = name.substring(probe.length());
      svc        = s;
      break;
    }
  }
  if (svc.isNull()) {
    return String("{\"error\":\"unknown service for tool: ") + name + "\"}";
  }
  JsonVariantConst ep = findEndpoint(svc, endpointId);
  if (ep.isNull()) {
    return String("{\"error\":\"unknown endpoint: ") + endpointId + "\"}";
  }

  const char *baseUrl = svc["baseUrl"] | "";
  const char *method  = ep["method"]   | "GET";
  const char *path    = ep["path"]     | "/";
  if (!*baseUrl) return "{\"error\":\"service missing baseUrl\"}";

  JsonDocument args;
  if (argsJson.length() > 0) {
    DeserializationError err = deserializeJson(args, argsJson);
    if (err) {
      return String("{\"error\":\"bad arguments: ") + err.c_str() + "\"}";
    }
  }

  String url = String(baseUrl) + path;
  X402Result r;

  if (strcmp(method, "GET") == 0) {
    // Flatten args into query string.
    bool first = true;
    JsonObjectConst argObj = args.as<JsonObjectConst>();
    if (!argObj.isNull()) {
      for (JsonPairConst kv : argObj) {
        url += first ? "?" : "&";
        first = false;
        url += kv.key().c_str();
        url += "=";
        url += urlEncode(jsonValueToString(kv.value()));
      }
    }
    Serial.printf("services: GET %s\n", url.c_str());
    r = x402Get(url);
  } else {
    String body;
    serializeJson(args, body);
    Serial.printf("services: POST %s (%u B)\n", url.c_str(), (unsigned)body.length());
    r = x402Post(url, body);
  }

  if (outCost) *outCost = r.costUsd;

  if (r.status != 200) {
    String err;
    err.reserve(128);
    err  = "{\"error\":\"HTTP ";
    err += String(r.status);
    if (r.error.length() > 0) {
      err += ": ";
      // Escape any quotes in the error string to keep valid JSON.
      String e = r.error;
      e.replace("\"", "\\\"");
      err += e;
    }
    err += "\"}";
    return err;
  }
  return r.body.length() > 0 ? r.body : String("{}");
}

#include "tools.h"
#include "services.h"
#include "x402.h"

// ---------------------------------------------------------------------------
// Name encoding — must match the Chrome extension's
//   `x402_${serviceId}_${endpointId}`
// so prompts and tool metadata stay compatible.
// ---------------------------------------------------------------------------
static String makeToolName(const String &svcId, const String &epId) {
  return String("x402_") + svcId + "_" + epId;
}

bool isToolName(const char *name) {
  return name && strncmp(name, "x402_", 5) == 0;
}

// Splits "x402_<serviceId>_<endpointId>" into (svc, ep). Service ids may
// contain dashes and underscores, so we split on the LAST underscore.
static bool splitToolName(const String &full, String &svc, String &ep) {
  if (!full.startsWith("x402_")) return false;
  int last = full.lastIndexOf('_');
  if (last <= 4) return false;
  svc = full.substring(5, last);
  ep  = full.substring(last + 1);
  return svc.length() > 0 && ep.length() > 0;
}

// ---------------------------------------------------------------------------
// Build the tools array for the LLM request.
// ---------------------------------------------------------------------------
void toolsBuildArray(JsonArray tools) {
  std::vector<Service> enabled = servicesEnabled();
  for (const Service &svc : enabled) {
    for (const ServiceEndpoint &ep : svc.endpoints) {
      JsonObject tool = tools.add<JsonObject>();
      tool["type"] = "function";

      JsonObject func = tool["function"].to<JsonObject>();
      func["name"]        = makeToolName(svc.id, ep.id);
      func["description"] = ep.description + " Paid via x402 in USDC (~" +
                            svc.priceRange + ").";

      JsonObject params   = func["parameters"].to<JsonObject>();
      params["type"]      = "object";
      JsonObject props    = params["properties"].to<JsonObject>();
      JsonArray  required = params["required"].to<JsonArray>();

      if (ep.params.empty()) {
        // Custom services may have no declared params; give the LLM a
        // generic input string slot it can fill in if needed.
        if (svc.custom) {
          JsonObject p = props["input"].to<JsonObject>();
          p["type"] = "string";
          p["description"] = "Input for this service. Read the description to know what to pass.";
        }
      } else {
        for (const ServiceParam &sp : ep.params) {
          JsonObject p = props[sp.name].to<JsonObject>();
          p["type"] = "string";                  // keep it simple
          p["description"] = sp.name;
          if (sp.required) required.add(sp.name);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// URL-encode a single query-parameter value.
// ---------------------------------------------------------------------------
static String urlEncodeValue(const String &in) {
  String out; out.reserve(in.length() + 8);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < in.length(); ++i) {
    uint8_t c = (uint8_t)in[i];
    bool safe = (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) out += (char)c;
    else { out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Execute a tool call.
// ---------------------------------------------------------------------------
String toolExecute(const char *toolName, const char *argsJson) {
  String svcId, epId;
  if (!splitToolName(String(toolName), svcId, epId)) {
    return String(R"({"error":"malformed tool name"})");
  }

  std::vector<Service> all = servicesAll();
  const Service *svc = serviceById(all, svcId);
  if (!svc) {
    return String("{\"error\":\"unknown service ") + svcId + "\"}";
  }
  const ServiceEndpoint *ep = nullptr;
  for (const ServiceEndpoint &e : svc->endpoints) {
    if (e.id == epId) { ep = &e; break; }
  }
  if (!ep) {
    return String("{\"error\":\"unknown endpoint ") + epId + "\"}";
  }

  // Parse LLM-supplied arguments.
  JsonDocument args;
  if (argsJson && *argsJson) {
    deserializeJson(args, argsJson);
  }

  // Unwrap the common {"input":"<json-string>"} pattern the LLM falls back
  // to when the tool schema only exposes a generic `input` parameter.
  // Mirrors the Chrome extension's executeServiceTool logic.
  JsonObjectConst argObj = args.as<JsonObjectConst>();
  if (!argObj.isNull() && argObj.size() == 1) {
    JsonVariantConst inp = args["input"];
    if (inp.is<const char*>()) {
      const char *s = inp.as<const char*>();
      if (s && *s == '{') {
        JsonDocument unwrapped;
        if (!deserializeJson(unwrapped, s) && unwrapped.is<JsonObject>()) {
          args = unwrapped;
          argObj = args.as<JsonObjectConst>();
        }
      }
    }
  }

  // Build URL + body per endpoint method.
  String url  = svc->baseUrl + ep->path;
  String body;
  X402Result res;
  auto pairToStr = [](JsonVariantConst v) -> String {
    if (v.is<const char*>())    return String(v.as<const char*>());
    if (v.is<bool>())           return v.as<bool>() ? "true" : "false";
    if (v.is<int>())            return String(v.as<int>());
    if (v.is<long>())           return String(v.as<long>());
    if (v.is<double>())         return String(v.as<double>());
    String s; serializeJson(v, s); return s;
  };

  if (ep->method == "GET") {
    String qs;
    if (!ep->params.empty()) {
      for (const ServiceParam &p : ep->params) {
        JsonVariantConst v = args[p.name.c_str()];
        if (v.isNull()) continue;
        String val = pairToStr(v);
        if (val.length() == 0) continue;
        qs += (qs.length() ? "&" : "?");
        qs += p.name + "=" + urlEncodeValue(val);
      }
    } else if (!argObj.isNull()) {
      for (JsonPairConst kv : argObj) {
        String val = pairToStr(kv.value());
        if (val.length() == 0) continue;
        qs += (qs.length() ? "&" : "?");
        qs += String(kv.key().c_str()) + "=" + urlEncodeValue(val);
      }
    }
    url += qs;
    res = x402Get(url);
  } else {
    // POST: forward the LLM's unwrapped args verbatim as the JSON body.
    // Our declared `ep->params` are only a HINT to the LLM about what
    // fields the endpoint accepts; different APIs use different casings
    // (username vs userName, token_id vs tokenId), so filtering the body
    // by declared names would drop correctly-cased fields the LLM chose
    // after seeing the service description.
    serializeJson(args, body);
    if (body.length() == 0 || body == "null") body = "{}";
    res = x402Post(url, body);
  }

  if (res.status != 200) {
    // Include a trimmed snippet of the server's response body so the LLM
    // has a chance to retry with the right arguments. Strip newlines and
    // cap at ~240 chars to keep the tool message small.
    String snippet = res.body;
    snippet.replace("\r", " ");
    snippet.replace("\n", " ");
    if (snippet.length() > 240) snippet = snippet.substring(0, 240) + "…";
    String msg = res.error.length() ? res.error : String("status ") + res.status;
    Serial.printf("tool %s: %s body=%s\n", toolName, msg.c_str(), snippet.c_str());
    // Escape quotes + backslashes for JSON embedding.
    snippet.replace("\\", "\\\\");
    snippet.replace("\"", "\\\"");
    return String("{\"error\":\"") + svc->name + " failed: " + msg +
           "\",\"body\":\"" + snippet + "\"}";
  }

  // Wrap the service's response the same way the Chrome extension does,
  // so the LLM sees a uniform shape.
  String wrapped;
  wrapped.reserve(res.body.length() + 80);
  wrapped  = "{\"source\":\""; wrapped += svc->name; wrapped += "\",";
  wrapped += "\"costUsd\":"; wrapped += String(res.costUsd, 5); wrapped += ",";
  wrapped += "\"data\":"; wrapped += (res.body.length() ? res.body : String("null"));
  wrapped += "}";
  return wrapped;
}

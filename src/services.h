// ============================================================================
//  x402 service catalog.
//
//  Built-in services are hardcoded here with the same data as the Chrome
//  extension's src/lib/x402-services.ts. Custom services (added by dropping
//  skill .md files into the Services tab) are stored as a JSON array in
//  NVS via devcfg, and merged in at runtime.
//
//  Every LLM call fans out to these services as OpenAI tools when the
//  user has them enabled in the Services tab.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

struct ServiceParam {
  String  name;
  bool    required;
};

struct ServiceEndpoint {
  String  id;
  String  description;    // what the LLM sees as the tool description
  String  method;         // "GET" or "POST"
  String  path;
  std::vector<ServiceParam> params;
};

struct Service {
  String  id;
  String  name;
  String  description;
  String  category;
  String  baseUrl;
  String  priceRange;
  bool    custom = false;
  std::vector<ServiceEndpoint> endpoints;
};

// Returns the merged list of built-in + user-added custom services.
std::vector<Service> servicesAll();

// Subset of servicesAll() filtered by the ids the user has toggled on.
std::vector<Service> servicesEnabled();

// Look up a single service by id. Returns nullptr if not found.
const Service *serviceById(const std::vector<Service> &list, const String &id);

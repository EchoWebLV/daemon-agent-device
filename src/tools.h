// ============================================================================
//  Tool-calling glue between the LLM and x402 services.
//
//  - toolsBuildArray: produces the JSON array that goes into the OpenAI
//    request body's `tools` field, based on which services the user has
//    toggled on in the Services tab.
//  - toolExecute: given a tool call name + arguments (both from the LLM
//    response), dispatches to the matching x402 service and returns the
//    response as a JSON string suitable for feeding back as a role=tool
//    message.
//
//  Tool names follow the Chrome extension's convention:
//      x402_<serviceId>_<endpointId>
//  (with dashes preserved — the extension splits on the last underscore).
// ============================================================================
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Populate `tools` (a JsonArray root) with one function definition per
// enabled endpoint. Safe to call even when no services are enabled — then
// the array stays empty.
void toolsBuildArray(JsonArray tools);

// True if this tool name looks like one of ours (starts with `x402_`).
bool isToolName(const char *name);

// Execute a tool call. Returns a JSON string with either `{source, costUsd,
// data}` on success or `{error}` on failure — passed back to the LLM as
// the content of a role=tool message.
String toolExecute(const char *toolName, const char *argsJson);

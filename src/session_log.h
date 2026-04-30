// ---------------------------------------------------------------------------
//  session_log — a small ring buffer of "what Claude just did" entries.
//
//  Claude Code calls daemon_log(text) over MCP, the device appends here, and
//  ai.c's system-prompt builder injects the joined list before every chat
//  call. That way when the user asks the device by voice "what did we just
//  do?", the on-device LLM already has the context to answer.
//
//  RAM-only for v1. Lost on reboot. NVS-backed persistence is a v2 concern.
//  Thread-safe: a mutex guards append/format because append runs on the
//  httpd task and format runs on the AI task.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>

#define SESSION_LOG_CAP     10
#define SESSION_LOG_MAX_LEN 200

// Initialises the mutex. Call once at boot, after nvs_flash_init().
void session_log_init(void);

// Append a one-line summary. Truncates to SESSION_LOG_MAX_LEN-1. Drops the
// oldest entry when the buffer is full.
void session_log_append(const char *text);

// Format the buffer into `out` as "- entry\n- entry\n...". Returns bytes
// written (excluding NUL), or 0 if the buffer is empty so callers can skip
// the prefix.
size_t session_log_format(char *out, size_t cap);

// Drop all entries. Currently unused; provided for future /log DELETE.
void session_log_clear(void);

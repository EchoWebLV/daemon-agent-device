# Session Log: voice-queryable record of what Claude just did

**Status:** Approved design, ready for implementation.
**Branch:** `feat/code-buddy`
**Date:** 2026-04-30
**Scope:** First step of the dev-buddy pivot. Lets the user ask the device by voice "what did we just do?" and get a useful answer, by having Claude Code feed the device a running log of significant actions through a new MCP tool.

---

## Goal

The user holds the front-face button, asks "hey, what did we just implement?", and the device speaks a coherent summary of recent work. The device must already have the context loaded, because at query time it cannot reach into the user's filesystem or git.

The mechanism is a ring buffer on the device that Claude Code populates via MCP, and that the chat path injects into the system prompt on every voice query.

---

## Architecture

```
Claude Code ──[stdio JSON-RPC]──> daemon-mcp.mjs ──[POST /log]──> device
                                                                   │
                                                                   ▼
                                                          session_log ring buffer
                                                                   │
User holds PTT ──> mic ──> stt ──> ai.build_system_prompt ─────────┘
                                       │
                                       ▼ (system prompt now includes recent actions)
                                   x402 chat ──> LLM
                                       │
                                       ▼
                                   ui_deliver_reply ──> voice ──> speaker
```

| Concern | Module |
|---|---|
| Ring buffer + accessors | new `src/session_log.c` / `src/session_log.h` |
| HTTP endpoint `POST /log` | new handler in `src/server.c` |
| System-prompt injection | new section in `build_system_prompt` in `src/ai.c` |
| MCP tool exposure | new tool entry in `tools/daemon-mcp.mjs` |

---

## Ring buffer

Lives in `session_log.c` as static state. RAM-only for v1 (lost on reboot). NVS-backed persistence is a v2 concern.

| Constant | Value | Why |
|---|---|---|
| `SESSION_LOG_CAP` | 10 entries | Enough context for a 30-60 minute coding stretch without bloating the system prompt |
| `SESSION_LOG_MAX_LEN` | 200 chars per entry | Forces Claude to write tight summaries; keeps total injection under ~2 KB |

Implementation: `char log_buf[SESSION_LOG_CAP][SESSION_LOG_MAX_LEN]`, plus `int count`. When full, drop the oldest and shift. A FreeRTOS mutex (`SemaphoreHandle_t`) guards the buffer because `session_log_append` is called from the httpd task and `session_log_format` from the AI task.

### API

```c
// session_log.h
void session_log_init(void);                       // create the mutex
void session_log_append(const char *text);         // copy + truncate, append
size_t session_log_format(char *out, size_t cap);  // join with newlines, returns bytes written
void session_log_clear(void);                      // reset (future: optional, called on /log DELETE)
```

`session_log_format` returns 0 if the buffer is empty so callers can skip the prefix entirely. Otherwise it writes:

```
- entry 1
- entry 2
- ...
```

with `\n` separators and a trailing `\0`.

---

## HTTP endpoint

`POST /log` mirrors the existing `/notify` handler shape exactly (cJSON parse, validate `text`, append, return ok).

Request:
```json
{ "text": "Implemented the session log ring buffer in src/session_log.c" }
```

Responses:
- `200 {"ok":true}` on success
- `400 {"error":"missing_text"}` if `text` is missing or empty
- `400 {"error":"bad_json"}` if the body isn't valid JSON
- `500 {"error":"oom"}` on allocation failure

No authentication. The device is already on a trusted local network; the MCP tool runs on the user's own machine.

---

## System-prompt injection

In `build_system_prompt` (in `ai.c`), after the wallet/price block but before the tool listing, add:

```c
n = strlen(out);
char log_block[2200];
size_t log_n = session_log_format(log_block, sizeof(log_block));
if (log_n > 0 && (size_t)n + log_n + 96 < cap) {
    snprintf(out + n, cap - n,
        "\nRECENT ACTIONS (most recent last) — Claude has been logging what "
        "it just did. Reference this when the user asks 'what did we do?', "
        "'what did you just implement?', or similar:\n%s\n",
        log_block);
}
```

The block is gated by capacity so it never overflows the 4096-byte system prompt buffer. With 10 entries × ~150 chars each, worst case is ~1500 bytes, well under the cap given the persona uses ~1200.

---

## MCP tool

In `tools/daemon-mcp.mjs`, add to the `TOOLS` object:

```js
daemon_log: {
  description:
    "Record a one-sentence summary of a meaningful step you just completed " +
    "(a feature shipped, a bug fixed, a decision made, a commit landed). " +
    "Describe WHAT was accomplished, not what command was run. " +
    "Examples: 'Added session log ring buffer to firmware', " +
    "'Fixed blockhash caching bug in x402.c', " +
    "'Decided against NVS persistence for v1'. " +
    "The user can later ask the Daemon by voice 'what did we just do?' " +
    "and the device replays your log entries as context.",
  inputSchema: {
    type: "object",
    properties: {
      text: {
        type: "string",
        description: "One sentence, under 200 chars, describing what was accomplished.",
      },
    },
    required: ["text"],
  },
  async run({ text }) {
    const r = await postJson("/log", { text });
    return r.ok ? `Logged: "${text}"` : JSON.stringify(r);
  },
},
```

The tool description is the prompt for Claude to use it. Keeping the description rich is the cheapest way to drive correct usage.

---

## Wiring

1. `app_main.c` calls `session_log_init()` once at boot, after NVS init.
2. `server.c` registers `POST /log -> handle_log` alongside the existing routes.
3. `ai.c` calls `session_log_format()` inside `build_system_prompt()`.
4. `daemon-mcp.mjs` exposes `daemon_log` as a tool.

The MCP server file is embedded into firmware via `scripts/embed_assets.py` and served at `GET /mcp.mjs`. So the new tool ships to the user's Claude Code install the next time they re-run `install.sh`.

---

## Failure modes

| Mode | Behavior |
|---|---|
| Buffer full | Oldest entry drops; new entry takes its slot. No back-pressure to caller. |
| Mutex contention | `session_log_append` blocks briefly. Worst case: a handful of ms. Acceptable on the httpd task. |
| Claude forgets to call `daemon_log` | Buffer has gaps. User asks "what did we do?" and gets only the entries Claude did log. Fix path: `CLAUDE.md` nudge. Out of scope for firmware. |
| Device reboots mid-session | Buffer empties. Acceptable for v1. NVS persistence is v2. |
| LLM hallucinates entries it didn't see | Possible because the prompt is open-ended. Mitigated by tight phrasing in the system-prompt header ("RECENT ACTIONS") and by the LLM's general adherence to provided context. |

---

## Out of scope (v2+)

- Persistent storage in NVS
- A `/log GET` endpoint that returns the buffer (debugging only; not user-facing)
- A `daemon_log_clear` tool to reset the buffer between sessions
- Auto-logging from firmware events (swap completed, wallet incoming) — the current ring is for Claude-driven dev work only
- Filtering / dedup (drop near-identical consecutive entries)
- A second buffer for "device action log" (the v2 reading I described in earlier brainstorm — what the device itself did)

---

## Test plan

1. Boot device, verify `session_log_init` runs without error.
2. From a laptop on the same network: `curl -X POST http://daemon.local/log -H 'Content-Type: application/json' -d '{"text":"hello world"}'` returns `{"ok":true}`.
3. Repeat 12 times with different texts; verify the first two drop off (oldest-first eviction).
4. Hold PTT, ask "what did we just do?", verify the device speaks a summary that references the logged entries.
5. From Claude Code, run a quick task, observe Claude calls `daemon_log` (visible in `claude mcp list`'s call history or via stderr logs from `daemon-mcp.mjs`).
6. Power-cycle the device, verify buffer empties (expected — v1 is RAM-only).

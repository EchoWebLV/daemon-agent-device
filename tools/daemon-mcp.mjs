#!/usr/bin/env node
// ---------------------------------------------------------------------------
//  Daemon MCP server — exposes the device as MCP tools to any MCP client
//  (Claude Code, Claude Desktop, etc).
//
//  Tools:
//    - daemon_notify(text)   Speak `text` on the device. Updates the LCD
//                            subtitle and TTS queue. Bypasses the LLM (no
//                            x402 cost, no chat history pollution).
//    - daemon_say(text)      Push `text` through the Daemon's chat path —
//                            the on-device LLM crafts a reply IN CHARACTER
//                            and speaks that. Costs USDC.
//    - daemon_state()        Returns connectivity + price + USDC balance
//                            so callers can show device health.
//
//  Transport: stdio (line-delimited JSON-RPC 2.0). Spawn this with
//  DAEMON_IP set in the environment, or pass --ip <addr>.
//
//  Self-contained — needs only Node 18+ (built-in fetch). No npm install.
// ---------------------------------------------------------------------------
import { createInterface } from "node:readline";

// ---- config ----------------------------------------------------------------
function parseArgIp() {
  const i = process.argv.indexOf("--ip");
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return null;
}
// Resolution order: --ip flag > DAEMON_IP env > daemon.local (mDNS).
// `daemon.local` works on Bonjour-aware hosts (macOS, most Linux with
// Avahi, Windows 10+). The firmware advertises this hostname as soon as
// Wi-Fi associates.
const DAEMON_HOST = parseArgIp() || process.env.DAEMON_IP || "daemon.local";
const DAEMON_BASE = `http://${DAEMON_HOST}`;

// ---- minimal logger (stderr only — stdout is sacred for JSON-RPC frames) ---
function log(level, ...args) {
  process.stderr.write(`[daemon-mcp ${level}] ${args.join(" ")}\n`);
}

// ---- HTTP helpers ----------------------------------------------------------
async function postJson(path, body) {
  if (!DAEMON_BASE) throw new Error("DAEMON_IP not configured");
  const res = await fetch(`${DAEMON_BASE}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const text = await res.text();
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${text || res.statusText}`);
  try { return JSON.parse(text); } catch { return { raw: text }; }
}

async function getJson(path) {
  if (!DAEMON_BASE) throw new Error("DAEMON_IP not configured");
  const res = await fetch(`${DAEMON_BASE}${path}`);
  const text = await res.text();
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${text || res.statusText}`);
  try { return JSON.parse(text); } catch { return { raw: text }; }
}

// ---- tool implementations --------------------------------------------------
const TOOLS = {
  daemon_notify: {
    description:
      "Speak a message on the Daemon device. Bypasses the on-device LLM, " +
      "so this is fast and free. Use for status updates like 'build done', " +
      "'tests passing', 'PR merged'. The text is spoken verbatim.",
    inputSchema: {
      type: "object",
      properties: {
        text: {
          type: "string",
          description: "The exact text to speak. Keep it short — under 200 chars.",
        },
      },
      required: ["text"],
    },
    async run({ text }) {
      const r = await postJson("/notify", { text });
      return r.ok ? `Spoken on Daemon: "${text}"` : JSON.stringify(r);
    },
  },

  daemon_say: {
    description:
      "Send a prompt through the Daemon's chat path — the on-device LLM " +
      "crafts a reply IN CHARACTER (Daemon's voice, with persona) and " +
      "speaks that. Costs USDC per call. Use for in-character reactions, " +
      "not raw status updates.",
    inputSchema: {
      type: "object",
      properties: {
        text: {
          type: "string",
          description: "What to ask Daemon. The on-device LLM rewrites it.",
        },
      },
      required: ["text"],
    },
    async run({ text }) {
      const r = await postJson("/say", { text });
      return r.reply || JSON.stringify(r);
    },
  },

  daemon_state: {
    description:
      "Read Daemon's current state: SOL price, USDC balance, online status. " +
      "Useful for health checks before posting status updates.",
    inputSchema: { type: "object", properties: {} },
    async run() {
      const r = await getJson("/state");
      return JSON.stringify(r);
    },
  },

  daemon_log: {
    description:
      "Record a one-sentence summary of a meaningful step you just " +
      "completed (a feature shipped, a bug fixed, a decision made, a " +
      "commit landed). Describe WHAT was accomplished, not what command " +
      "was run. Examples: 'Added session log ring buffer to firmware', " +
      "'Fixed blockhash caching bug in x402.c', 'Decided against NVS " +
      "persistence for v1'. The user can later ask the Daemon by voice " +
      "'what did we just do?' and the device replays your log entries " +
      "as context. Call this proactively after every meaningful step. " +
      "Free, no USDC cost.",
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
};

// ---- JSON-RPC dispatch -----------------------------------------------------
function send(obj) {
  process.stdout.write(JSON.stringify(obj) + "\n");
}

function reply(id, result) {
  if (id == null) return;            // notifications don't get a response
  send({ jsonrpc: "2.0", id, result });
}

function replyError(id, code, message) {
  if (id == null) return;
  send({ jsonrpc: "2.0", id, error: { code, message } });
}

async function handle(msg) {
  const { id, method, params } = msg;

  switch (method) {
    case "initialize":
      reply(id, {
        protocolVersion: "2024-11-05",
        capabilities: { tools: {} },
        serverInfo: { name: "daemon", version: "0.1.0" },
      });
      break;

    case "notifications/initialized":
      // Client done with handshake. No response needed.
      break;

    case "tools/list":
      reply(id, {
        tools: Object.entries(TOOLS).map(([name, t]) => ({
          name,
          description: t.description,
          inputSchema: t.inputSchema,
        })),
      });
      break;

    case "tools/call": {
      const name = params?.name;
      const args = params?.arguments || {};
      const tool = TOOLS[name];
      if (!tool) {
        replyError(id, -32602, `Unknown tool: ${name}`);
        return;
      }
      try {
        const out = await tool.run(args);
        reply(id, {
          content: [{ type: "text", text: typeof out === "string" ? out : JSON.stringify(out) }],
        });
      } catch (e) {
        reply(id, {
          isError: true,
          content: [{ type: "text", text: `Error: ${e.message}` }],
        });
      }
      break;
    }

    case "ping":
      reply(id, {});
      break;

    default:
      // Unknown methods are softly errored so capability sniffing doesn't
      // explode the connection. Notifications (id == null) get nothing.
      if (id != null) replyError(id, -32601, `Method not found: ${method}`);
  }
}

// ---- main loop -------------------------------------------------------------
const rl = createInterface({ input: process.stdin });
rl.on("line", async (line) => {
  if (!line.trim()) return;
  let msg;
  try { msg = JSON.parse(line); }
  catch (e) { log("error", "bad JSON:", e.message); return; }
  try { await handle(msg); }
  catch (e) { log("error", "handler crashed:", e.message); }
});

rl.on("close", () => process.exit(0));

log("info", `started; talking to ${DAEMON_BASE || "(unset)"}`);

---
description: Show daemon device status, recent actions, and what tools the MCP exposes.
---

Give me a quick status report on the daemon device.

1. Call the `daemon_state` MCP tool to fetch current SOL price, USDC balance, and online state. If the tool call errors, say "device unreachable" instead of guessing.

2. List the daemon MCP tools that are currently available in this session, one bullet each, with the tool name in code, a one-sentence description of what it does, and the cost (free or USDC). Only list tools that exist — do not invent any.

3. End with one line of guidance about how to add a new entry to the device's session log, e.g. `daemon_log("Implemented X")`.

Keep the whole response compact: roughly fifteen lines of markdown, no preamble or sign-off.

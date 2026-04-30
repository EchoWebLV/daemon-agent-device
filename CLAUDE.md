# Daemon project notes for Claude

## Always log meaningful steps to the device

After each meaningful step in this project, call the `daemon_log` MCP tool with one sentence describing what was accomplished. The device's chat path replays these entries when the user asks "what did we just do?" by voice, so without them the on-device LLM has nothing to answer from and will hallucinate.

What counts as a meaningful step:
- A feature shipped or a bug fixed
- A decision made (chose approach A over B)
- A commit landed
- A non-trivial diagnosis (e.g., "found PTT regression caused by I2C race")

What does **not** count (skip these):
- Reading a file
- Running a single grep
- Stating an intent before doing the work
- Routine builds or restarts

Phrasing: describe the *outcome*, not the command. "Implemented session log ring buffer in firmware" beats "Ran Edit on session_log.c". Keep entries under 200 characters.

The tool is free (no USDC cost). Call it freely. Quality of the entries directly determines how useful the voice recap is.

"""Pre-build step: embed static assets as regular C source files.

PlatformIO's ESP-IDF builder advertises `board_build.embed_txtfiles`, but
the generated .S file lands at a path SCons can't find (the target gets a
doubled prefix: `.pio/build/<env>/.pio/build/<env>/index.html.S.o`).
Sidestepping that by generating a normal .c file here.

Emits, for each asset, a pair of symbols:

    extern const char   <sym>_start[];   // the raw bytes (no trailing NUL)
    extern const size_t <sym>_len;        // byte length

so server.c can use them like:

    httpd_resp_send(req, index_html_start, index_html_len);

The generator only rewrites the output when the source is newer, so
incremental builds stay fast.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable

Import("env")  # noqa: F821  (injected by PlatformIO SCons)

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
OUT_DIR     = PROJECT_DIR / "src" / "generated"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# (source path relative to project root, C symbol base)
ASSETS: list[tuple[str, str]] = [
    ("src/html/index.html",     "index_html"),
    # Daemon MCP server source — embedded so any device owner can curl it
    # straight off the device (GET /mcp.mjs) and configure Claude Code
    # without needing the repo.
    ("tools/daemon-mcp.mjs",    "mcp_mjs"),
]


def _render_bytes(data: bytes, per_line: int = 16) -> Iterable[str]:
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        yield "    " + ", ".join(f"0x{b:02x}" for b in chunk) + ","


def _generate_one(src_rel: str, sym_base: str) -> Path:
    src = PROJECT_DIR / src_rel
    dst = OUT_DIR / f"{sym_base}.c"

    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
        return dst

    data  = src.read_bytes()
    lines = list(_render_bytes(data))
    content = (
        f"// Generated from {src_rel} by scripts/embed_assets.py — do not edit.\n"
        f"// Matches the convention in server.c:\n"
        f"//   extern const char   {sym_base}_start[];\n"
        f"//   extern const size_t {sym_base}_len;\n\n"
        f"#include <stddef.h>\n\n"
        f"const char {sym_base}_start[] = {{\n"
        + "\n".join(lines)
        + f"\n}};\n\n"
        f"const size_t {sym_base}_len = {len(data)};\n"
    )
    dst.write_text(content)
    return dst


generated: list[str] = []
for src_rel, sym_base in ASSETS:
    generated.append(str(_generate_one(src_rel, sym_base).relative_to(PROJECT_DIR)))

print("embed_assets.py: " + ", ".join(generated))

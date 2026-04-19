#!/usr/bin/env python3
"""
Talk to Daemon with your laptop microphone.

Flow:
  press Enter  → record ~6 s of audio from the default mic
              → POST to Gemini generateContent as inline audio
              → POST the transcript to the board's /say endpoint
              → Daemon thinks (x402-paid LLM) and speaks through its speaker

No browser. No Chrome permissions. No proxy. Just Python and your mic.

Config (all optional, defaults work if you run this from the repo root):
  DAEMON_HOST       http://172.20.10.2        board IP
  GEMINI_API_KEY    <from env or src/secrets.h>
  GEMINI_MODEL      gemini-2.0-flash
  RECORD_SECONDS    6
"""
from __future__ import annotations

import base64
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request
import wave

try:
    import sounddevice as sd
    import numpy as np
except ImportError:
    sys.stderr.write(
        "missing deps. run once:\n"
        "    python3 -m pip install --user sounddevice numpy\n"
    )
    sys.exit(1)

SR               = 16000
RECORD_SECONDS   = int(os.environ.get("RECORD_SECONDS", "6"))
BOARD            = os.environ.get("DAEMON_HOST", "http://172.20.10.2")
GEMINI_KEY       = os.environ.get("GEMINI_API_KEY") or ""
GEMINI_MODEL     = os.environ.get("GEMINI_MODEL") or ""

# Fall back to reading the keys out of src/secrets.h so you don't have to
# duplicate them into the environment. Only runs if the env vars aren't set.
def _load_secrets() -> None:
    global GEMINI_KEY, GEMINI_MODEL
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "src", "secrets.h")
    if not os.path.isfile(path):
        return
    with open(path) as f:
        for line in f:
            if not GEMINI_KEY:
                m = re.match(r'\s*#define\s+GEMINI_API_KEY\s+"([^"]+)"', line)
                if m:
                    GEMINI_KEY = m.group(1)
            if not GEMINI_MODEL:
                m = re.match(r'\s*#define\s+GEMINI_MODEL\s+"([^"]+)"', line)
                if m:
                    GEMINI_MODEL = m.group(1)
_load_secrets()
if not GEMINI_MODEL:
    GEMINI_MODEL = "gemini-2.0-flash"
if not GEMINI_KEY:
    sys.stderr.write("missing GEMINI_API_KEY (env or src/secrets.h)\n")
    sys.exit(1)


# ---------- mic capture -----------------------------------------------------
def record(seconds: int) -> np.ndarray:
    sys.stdout.write(f"recording {seconds}s... speak now.\n")
    sys.stdout.flush()
    audio = sd.rec(int(seconds * SR), samplerate=SR, channels=1, dtype="int16")
    sd.wait()
    return audio.flatten()


# ---------- WAV wrapper -----------------------------------------------------
def pcm_to_wav(pcm: np.ndarray) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    return buf.getvalue()


# ---------- Gemini STT ------------------------------------------------------
def transcribe(pcm: np.ndarray) -> str | None:
    wav = pcm_to_wav(pcm)
    body = {
        "contents": [{
            "parts": [
                {"text": (
                    "Transcribe the user's spoken audio. Return ONLY the "
                    "literal spoken words, with no quotes, commentary, or "
                    "translation. If there is no clear speech, return an "
                    "empty string."
                )},
                {"inline_data": {
                    "mime_type": "audio/wav",
                    "data": base64.b64encode(wav).decode("ascii"),
                }},
            ],
        }],
        "generationConfig": {"temperature": 0, "maxOutputTokens": 256},
    }
    url = (
        "https://generativelanguage.googleapis.com/v1beta/models/"
        f"{GEMINI_MODEL}:generateContent?key={GEMINI_KEY}"
    )
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.loads(r.read())
    except urllib.error.HTTPError as e:
        payload = e.read()[:300].decode(errors="replace")
        sys.stderr.write(f"gemini http {e.code}: {payload}\n")
        return None
    except Exception as e:
        sys.stderr.write(f"gemini error: {e}\n")
        return None

    try:
        return data["candidates"][0]["content"]["parts"][0]["text"].strip()
    except (KeyError, IndexError):
        sys.stderr.write(f"gemini unexpected response: {json.dumps(data)[:300]}\n")
        return None


# ---------- board ----------------------------------------------------------
def send_to_board(text: str) -> bool:
    req = urllib.request.Request(
        BOARD + "/say",
        data=text.encode("utf-8"),
        method="POST",
        headers={"Content-Type": "text/plain"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status in (200, 202)
    except Exception as e:
        sys.stderr.write(f"board error: {e}\n")
        return False


# ---------- driver ---------------------------------------------------------
def main() -> None:
    sys.stdout.write(
        f"daemon voice chat\n"
        f"  board : {BOARD}\n"
        f"  model : {GEMINI_MODEL}\n"
        f"  length: {RECORD_SECONDS}s per utterance\n"
        f"press Enter to talk. ctrl-c to quit.\n"
    )
    sys.stdout.flush()

    while True:
        try:
            input()
        except (KeyboardInterrupt, EOFError):
            print()
            return

        pcm = record(RECORD_SECONDS)
        if pcm.size < SR // 4:
            sys.stdout.write("  too short, skipped\n")
            continue

        peak = int(np.abs(pcm).max()) if pcm.size else 0
        sys.stdout.write(f"  captured {pcm.size} samples (peak={peak})\n")

        if peak < 500:
            sys.stdout.write("  nothing audible, skipped\n")
            continue

        sys.stdout.write("  transcribing...\n")
        sys.stdout.flush()
        text = transcribe(pcm)
        if not text:
            sys.stdout.write("  no transcript\n")
            continue

        sys.stdout.write(f"you  : {text}\n")
        sys.stdout.flush()
        if send_to_board(text):
            sys.stdout.write("  daemon is thinking + will speak in a moment\n")
        else:
            sys.stdout.write("  send to board failed\n")


if __name__ == "__main__":
    main()

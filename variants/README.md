# voice.cpp variants

Swap-in implementations of `src/voice.cpp`. This folder is **not** compiled
by PlatformIO — these files live outside `src/` on purpose. To activate one,
copy it over `src/voice.cpp` and rebuild.

| File                      | What it does                                           | When to use                                                        |
| ------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------ |
| `voice_piper.cpp`         | Local Piper on LAN, ElevenLabs fallback on error.      | Home / at-desk. Piper server must be reachable at `PIPER_HOST`.    |
| `voice_elevenlabs.cpp`    | Direct to ElevenLabs over HTTPS, no Piper code at all. | Travel / cellular / any network where the Mac isn't reachable.    |

Both variants share the same optimizations: two alternating MP3/WAV slot
files (no stop-wait stall), reused `WiFiClientSecure` + `HTTPClient` with
HTTP/1.1 keep-alive, `mp3_22050_32` + `optimize_streaming_latency=4` on the
ElevenLabs URL.

## Swap to Piper

```bash
cp variants/voice_piper.cpp src/voice.cpp
pio run -t upload --upload-port /dev/cu.usbmodem1101
```

Make sure the Piper HTTP server is up on the Mac:

```bash
launchctl kickstart -k gui/$UID/com.yordan.piper-tts
curl -s -o /dev/null -w "%{http_code}\n" -X POST \
  http://172.20.10.3:5500/ -H 'Content-Type: application/json' \
  -d '{"text":"ok"}'
```

`PIPER_HOST` + `PIPER_PORT` are defined in `src/secrets.h`. Leave them
set even while running `voice_elevenlabs.cpp` — they're only read when
the Piper variant is active.

## Swap to ElevenLabs-only

```bash
cp variants/voice_elevenlabs.cpp src/voice.cpp
pio run -t upload --upload-port /dev/cu.usbmodem1101
```

## Keeping the variants in sync

When you edit `src/voice.cpp` directly, remember to also update whichever
variant here it came from so the snapshot stays accurate. A quick way:

```bash
# after editing src/voice.cpp as the Piper variant
cp src/voice.cpp variants/voice_piper.cpp

# after editing src/voice.cpp as the ElevenLabs-only variant
cp src/voice.cpp variants/voice_elevenlabs.cpp
```

Then re-add the banner comment at the top of the variant file so it's
clear which flavor it is when you come back to it later.

## Why a folder outside `src/`?

PlatformIO's default source filter recursively compiles every `.c`/`.cpp`
file under `src/`. Two `voice.cpp` implementations in the same build tree
would produce duplicate symbols at link time. Keeping variants here is the
simplest way to stash alternate implementations without touching
`platformio.ini` or juggling build flags.

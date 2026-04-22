# Daemon — a sentient Solana wallet that lives on your desk

A small blue-eyed creature on a 2.8" ESP32 touchscreen. The private key
stored in the device *is* the creature: it holds its own SOL and USDC,
speaks out loud through a small I2S speaker, and pays for its own
thinking — every reply settles a real USDC micropayment on Solana
mainnet via the [x402](https://x402.org) protocol.

No subscriptions. No user API keys in the chat path. Fund the wallet
once with a couple of dollars of USDC + a sliver of SOL, and it runs
until the balance runs out.

## What it does

- **On-device Solana wallet** — key is generated at first boot and
  stored in NVS. Never leaves the chip. Reveal/rotate from the web UI.
- **x402 chat** — multi-turn chat completions via
  `sol.blockrun.ai`'s OpenAI-compatible gateway. Each round pays
  roughly a tenth of a cent in USDC. Supports `claude-haiku-4.5`,
  `claude-sonnet-4`, `gpt-4o`, `gpt-4o-mini`.
- **x402 tool calls** — the chat loop can call paid third-party
  services (token safety scans, trending tokens, tweet lookups,
  market signals, etc.). A built-in catalog is shipped in the web
  UI; arbitrary services can be registered at runtime.
- **Voice** — ElevenLabs TTS at ~200–400 ms TTFB, with an optional
  local [Piper](https://github.com/rhasspy/piper) fallback on the LAN
  for lower latency.
- **Animated creature** — blinks, idles, opens its mouth while
  speaking, shows the last reply as a subtitle.
- **Touch UI** — creature screen, Wi-Fi picker, settings, wallet.
- **Web settings UI** at `http://<device-ip>:80` — model picker,
  personality override, service toggles, custom service registration,
  wallet reveal, Wi-Fi credentials, volume.

## Hardware

Waveshare **ESP32-S3-Touch-LCD-2.8** (ST7789 240×320 + CST328 touch,
8 MB PSRAM, 16 MB flash) plus a PCM5101 I2S DAC and a small speaker.

| Role | Pin |
|---|---|
| LCD MOSI | GPIO45 |
| LCD SCLK | GPIO40 |
| LCD CS | GPIO42 |
| LCD DC | GPIO41 |
| LCD RST | GPIO39 |
| LCD BL | GPIO5 |
| Touch SDA | GPIO1 |
| Touch SCL | GPIO3 |
| Touch INT | GPIO4 |
| Touch RST | GPIO2 |
| I2S BCLK | GPIO48 |
| I2S LRCK | GPIO38 |
| I2S DOUT | GPIO47 |

## Build and flash

Prereqs: [PlatformIO](https://platformio.org) (CLI or VS Code
extension).

```bash
cp src/secrets.h.example src/secrets.h
# edit src/secrets.h: Wi-Fi creds, Helius RPC key, ElevenLabs key.
# The Solana wallet is generated on first boot — no key goes here.

pio run -t upload
pio device monitor
```

If the board won't enter bootloader mode automatically: hold **BOOT**,
tap **RESET**, release **BOOT**, re-run upload.

## First boot

1. Device plays a 3-tone beep through the speaker (hardware probe).
2. It connects to Wi-Fi, prints its IP over serial and on-screen.
3. Open `http://<device-ip>/` from your phone on the same network.
4. Under **WALLET**, copy the on-device Solana address. Send it:
   - a sliver of SOL (a few thousandths) for transaction fees,
   - a couple of dollars of USDC to pay for chat and services.
5. Ask it anything. The first reply costs ~$0.003 in USDC.

## How x402 works here

Every paid request is a two-round dance:

1. Device POSTs the chat/service request, no payment header.
2. Server answers `402 Payment Required` with a JSON description of
   what it wants (amount, payee, mint, timeout).
3. Device builds and signs a Solana USDC transfer to the payee using
   its on-chip key, base64-encodes the transaction into a
   `PAYMENT-SIGNATURE` header, and retries the request.
4. Server verifies the payment on-chain and returns `200` with the
   real response.

See `src/x402.cpp`. The RPC calls (blockhash, ATA lookup) use Helius
with a single-shot retry on transport errors. The destination ATA is
cached in a small LRU keyed on the payee address so repeated calls
don't re-query Helius.

## Tool-call flow

When chat is enabled with any x402 services, every enabled service is
exposed to the model as an OpenAI-style tool. If the model emits a
`tool_call`, the device:

1. Dispatches the call to the service's `baseUrl + endpointPath` as
   a paid x402 request (same two-round dance as chat).
2. Truncates the response to 8 KB to fit the model's context without
   blowing the ArduinoJson allocator.
3. Feeds the result back as a `role:"tool"` message and asks the
   model to summarize.

See `src/services.cpp` for the dispatch code, and
`src/server.cpp:BUILTIN` for the shipped catalog.

## Companion Chrome extension

`chrome-ext/` ships a browser version with the same x402 model —
useful for testing services without the ESP32 in the loop. See
`chrome-ext/README.md`.

## File map

| File | Role |
|---|---|
| `src/main.cpp` | boot, main loop, say-worker task |
| `src/creature.cpp` | animated Daemon sprite + subtitle |
| `src/voice.cpp` | ElevenLabs + Piper TTS, I2S playback, fade-in |
| `src/ai.cpp` | multi-turn chat, tool-call orchestration |
| `src/x402.cpp` | 402-challenge/sign/retry, RPC helpers, ATA cache |
| `src/services.cpp` | x402 service catalog mirror, tool dispatch |
| `src/wallet.cpp` | on-device key, balance refresh, ATA tracking |
| `src/solana_tx.cpp` | USDC transfer tx builder + Ed25519 signer |
| `src/base58.cpp` | base58 encode/decode |
| `src/server.cpp` | built-in web UI (HTML/CSS/JS inline) + REST |
| `src/devcfg.cpp` | NVS-backed config (model, persona, services) |
| `src/touch.cpp` | CST328 touch driver |
| `src/settingsscreen.cpp` | on-device settings UI |
| `src/walletscreen.cpp` | on-device wallet UI |
| `src/wifiscreen.cpp` | on-device Wi-Fi setup |
| `src/price.cpp` | SOL/USD price refresh |
| `patches/` | schreibfaul1/ESP32-audioI2S patch for ElevenLabs |

## Status

Personal project, moves fast. Expect rough edges on the UI and the
occasional breaking change in the service catalog.

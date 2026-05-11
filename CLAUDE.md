# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for **Daemon**, a Solana-wallet-bearing creature that lives on an ESP32-S3-BOX-3 (240x320 ILI9342C LCD + GT911 touch + ES8311 speaker codec + ES7210 mic). Pure C on **ESP-IDF only** (no Arduino layer). Every spoken reply settles a real USDC micropayment over the x402 protocol against `sol.blockrun.ai`. Skills can be uploaded as markdown to a LittleFS partition and exposed to the on-device LLM as OpenAI-style tool calls.

The top-level `README.md` is **stale** (references the previous Waveshare board and `.cpp` files). The actual hardware and language are described above; trust the source over the README until that file is refreshed.

## Build, flash, run

Toolchain: PlatformIO with the ESP-IDF platform pinned in `platformio.ini`. There is only one env: `esp32_s3_box_3b` (legacy name; the actual hardware is BOX-3, not BOX-3B).

```bash
# First time only
cp src/secrets.h.example src/secrets.h    # then edit: WIFI, HELIUS, ELEVENLABS, OPENAI
pio run                                    # compile
pio run -t upload                          # flash over USB-CDC (no BOOT/RESET dance; usb_reset hook is wired)
pio device monitor                         # 115200 baud, USB-CDC console
pio run -t erase && pio run -t upload      # nuke NVS + LittleFS partitions if a stored config is bricking boot
```

The web UI lives at `src/html/index.html` and the MCP shim at `tools/daemon-mcp.mjs`. Both are embedded into the binary by `scripts/embed_assets.py`, a PIO **pre-build hook** that emits `src/generated/*.c` containing `<sym>_start[]` and `<sym>_len`. Do not hand-edit `src/generated/`; edit the sources and rebuild. Built-in `board_build.embed_txtfiles` is intentionally avoided (a PIO+SCons path bug doubles the target prefix).

## Tests

There is no unit-test framework. The test suite is a single Python driver that talks to a firmware-side harness over USB-CDC.

```bash
pip install -r tests/requirements.txt
python tests/run.py                  # full run (~45 s, includes 3 paid x402 cases)
python tests/run.py --skip-x402      # ~10 s, no real USDC spent
python tests/run.py --only wifi      # substring filter
python tests/run.py --port /dev/cu.usbmodem2101 --rpc https://mainnet.helius-rpc.com/?api-key=...
```

The host sends `TEST BEGIN\n` followed by verb lines. The device side is always compiled in (`src/testharness.c`; entry point `test_harness_begin()` in `app_main.c`) and idles on `fgets()` with no runtime cost outside an active run. Protocol and case list: `docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md`. Preconditions: device flashed, Wi-Fi joined once before, wallet funded with at least ~0.001 SOL and $0.05 USDC.

`tools/smoke_tasks.py <ip>` is a separate over-Wi-Fi smoke test for the `/api/tasks` scheduler.

## Architecture (the parts that span files)

### Boot order matters
`app_main.c` is the single sequencing point. The order is load-bearing:
1. `nvs_flash_init` (with one-shot erase-and-retry on corruption) before any reader.
2. `devcfg_init` brings the backlight LEDC PWM online at the stored duty.
3. `skill_store_init` mounts LittleFS at `/storage` for markdown skill bodies. Non-fatal on failure.
4. `tls_lock_init` MUST run before any subsystem that opens HTTPS. See below.
5. `display_init` before `touch_init` because the GT911 reset line is level-shifted off `LCD_RST` (GPIO48). A panel reset after the touch IC is up would yank the touch controller out from under LVGL.
6. `ui_init` then `touch_init` then `ui_tune_gestures` (the gesture threshold tune needs the indev created by touch_init).
7. `wifi_sta_begin` is non-fatal on failure; offline mode still works.
8. `server_start` and `mdns_init` only after Wi-Fi associates (advertises `daemon.local`).
9. `ai_begin`, `voice_begin`, `mic_init`, `wake_init`, `buttons_init`, `wallet_begin`, `price_begin` in that order. `wake_init` owns the ES7210 codec continuously and supersedes `mic.c`'s old open-on-PTT lifecycle.

### Global TLS mutex (`tls_lock`)
The ESP32-S3 has ~320 KB of internal RAM and mbedtls AES contexts must live there. Hardware AES is therefore deliberately **disabled** (see `sdkconfig.defaults`) and mbedtls allocations are routed to PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`). Even so, concurrent TLS handshakes from wallet refresh + price refresh + AI + TTS + STT + x402 + scheduler will race on internal-RAM allocs and the second one crashes. **Every outbound HTTPS call takes `tls_lock_take` before opening the connection and `tls_lock_give` after closing.** This is non-negotiable; any new TLS path must wire through it.

### x402 payment dance
`src/x402.c` owns the "402 -> sign USDC transfer -> retry with `PAYMENT-SIGNATURE` header" round trip. Public surface: `x402_call`, `x402_post`, `x402_call_stream`, plus `x402_fetch_recent_blockhash` and `x402_fetch_usdc_ata` exposed for skills that need to build their own payments. The destination ATA is LRU-cached per payee. **Do not cache the recent blockhash across calls.** Re-using a blockhash produces a duplicate-signature transaction, the facilitator returns 402 again, and the reply path breaks. Fetch fresh per call.

### Skill loader (markdown-bodied services)
The same `svc_custom` (NVS JSON) and `svc_enabled` (toggle list) plumbing now carries two kinds of service entries discriminated by `kind`: existing structured services and new markdown skills. Skill markdown bodies live at `/storage/skills/<id>.md` on LittleFS (NVS holds only lightweight metadata + declared credential names; per-skill secrets at `/storage/skills/<id>.creds.json`). Five generic tools are exposed to the LLM whenever any markdown skill is enabled: `http_request`, `x402_pay`, `secret_get`, `secret_set`, `solana_get_pubkey` (see `src/skill_tools.{c,h}`; dispatched from `src/ai.c`). The full design is in `docs/superpowers/specs/2026-05-09-skill-loader-design.md`. SP3ND (Amazon/eBay over USDC) is the first target skill.

### LVGL on esp_lcd
LVGL v9 + `esp_lvgl_port` glue. LCD is on **SPI3**, not SPI2 (legacy Waveshare wiring used SPI2; any stray `SPI2_HOST` will silently route pixels to the wrong pins). The four screens (creature / wallet / settings / wifi, plus menu / info / config / swap / x_post / config) are owned by `ui.c` and the individual `*_screen.c` files. Anything that mutates visible state must hold the `lvgl_port` lock or run from inside an LVGL event callback (which implicitly holds it).

### Wake word + audio
`wake.c` owns the AFE pipeline continuously and serves both always-on wake detection and PTT capture (via `wake_capture_start/stop` behind the legacy `mic_record_start/stop`). The selected WakeNet model is `CONFIG_SR_WN_WN9_HIESP` (placeholder "Hi ESP") in `sdkconfig.defaults` because the custom "Hey Daemon" model is in flight at Espressif's generator. When the custom model arrives, swap that symbol and update `partitions.csv` if the model bin grows past the 2 MB `model` partition. **Keep the wake-word path service-agnostic**: both Espressif's official tool and `custom-espsr.com` produce compatible WN9 models, and we want either to work.

### Partition table (custom, 16 MB flash)
See `partitions.csv`:
- `nvs` 24 KB at 0x9000: wifi creds, devcfg, wallet keypair, skill metadata.
- `factory` 5 MB at 0x10000: app.
- `model` 2 MB SPIFFS at 0x510000: ESP-SR WakeNet + VAD models. Loaded by `esp_srmodel_init("model")`.
- `storage` ~9 MB LittleFS at 0x710000: skill markdown bodies + creds, TTS cache, logs.

## Conventions worth knowing

- All `.c`/`.h` files live in `src/`. Headers begin with a multi-line `// ----` comment explaining the module's role and any non-obvious constraints. Match that style when adding files; it is the primary documentation surface.
- `src/board.h` is the single source of truth for GPIO assignments. Lifted from Espressif's `esp-bsp/bsp/esp-box-3` source. Audio + I2C pins are owned by the BSP and intentionally not redeclared.
- `src/secrets.h` is gitignored. Edit `src/secrets.h.example` if you need to update the template.
- The `.claude/`, `.codex/`, and `training/` directories are gitignored local scratch.
- Design specs and execution plans live under `docs/superpowers/specs/` and `docs/superpowers/plans/`. When implementing a new feature with a spec, follow the spec rather than re-deriving the plan.
- The hardware is BOX-3 (GT911 touch), NOT BOX-3B (TT21100). The PIO env name and the comment in `platformio.ini` still say "BOX-3B" for historical reasons; the actual code in `display.c` / `touch.c` is correct for BOX-3.

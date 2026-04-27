# ESP32-S3-BOX-3B migration

## Goal

Move the entire Daemon firmware from the **Waveshare ESP32-S3-Touch-LCD-2.8**
to the **ESP32-S3-BOX-3B**. Replace the Waveshare PIO env entirely (no dual-board
support). Keep the application logic (Solana wallet, x402, chat, web UI)
untouched; swap every hardware-touching subsystem to BOX-3B silicon, and
re-lay-out all 8 LVGL screens for the BOX-3B's native landscape orientation.

This is a port, not a re-flash. Display, touch, audio (in + out), and pin map
all change.

## Hardware delta

| Subsystem | Waveshare 2.8" (today) | ESP32-S3-BOX-3B (target) |
|---|---|---|
| MCU | ESP32-S3, 8 MB Quad PSRAM, 16 MB flash | ESP32-S3, 8 MB **Octal** PSRAM, 16 MB flash |
| Display | ST7789, 240×320 portrait, SPI | ILI9342C, 320×240 landscape, SPI |
| Touch | CST328 (Waveshare driver), I2C bus dedicated | TT21100 (Espressif driver), I2C bus shared with audio codec |
| Audio out | I2S → PCM5101 DAC → speaker | I2S → ES8311 codec → PA → speaker |
| Audio in | (none) | I2S ← ES7210 codec ← dual digital mics |
| IMU | QMI8658 on dedicated I2C bus, used for shake-to-talk | none |
| Buttons | none | 1 BOOT/CONFIG button (GPIO0) + 1 MUTE slide switch (GPIO1) |
| IR | none | none (BOX-3 has neither IR rx nor tx — verified absent from official BSP) |

## Decisions

These are settled — recorded here so the plan and implementation don't
re-litigate them.

1. **Single-board target.** Replace `[env:waveshare_esp32s3_28]` with
   `[env:esp32_s3_box_3b]`. No `#ifdef` board-selector. If a Waveshare needs
   to be re-flashed, branch off the last commit before this migration.
2. **Hybrid driver strategy.** Keep one-C-file-per-subsystem layout. Swap to
   BOX-3B silicon via individual managed components (see *Managed components*
   below), not the full `esp_bsp_box_3` package. Existing init flow in
   `app_main.c` is preserved with new function names slotted into the same
   pattern.
3. **Landscape 320×240.** All 8 LVGL screens re-laid-out for the BOX-3B's
   native orientation. No portrait-on-its-side hack.
4. **Voice input scope = bring-up only.** ES7210 mic comes alive and
   `mic_read()` is exposed as an API. No STT, no wake-word, no app consumer
   in this migration. Push-to-talk and `esp-sr` wake-word are explicit
   follow-ups.
5. **Drop `imu.c`.** Shake-to-talk is gone. The voice-input modal it opened
   becomes orphaned UI in this branch and is re-wired by the future
   push-to-talk branch.
6. **Smoke-test wiring for new hardware.**
   - **BOOT button (GPIO0)** → "back / pop screen" via existing screen stack. Press during running app is safe; bootloader entry only happens if held during reset.
   - **MUTE switch (GPIO1)** → state-change log to serial only. (It's a slide switch, not a momentary button — we log every transition.)
   - **Mic** → 1 Hz RMS-level task logs to serial.
   - **No IR work** — BOX-3 has no IR hardware. Out of scope.

## Architecture

### File-level diff

| Path | Change |
|---|---|
| `platformio.ini` | Replace `[env:waveshare_esp32s3_28]` with `[env:esp32_s3_box_3b]`. Same chip, new build flags + sdkconfig file. |
| `sdkconfig.defaults` | Octal PSRAM (was Quad), keep USB-Serial/JTAG console. |
| `sdkconfig.waveshare_esp32s3_28` → **`sdkconfig.esp32_s3_box_3b`** | Renamed + regenerated for new env. |
| `partitions.csv` | Unchanged (16 MB layout fits). |
| `src/idf_component.yml` | Drop `waveshare/esp_lcd_touch_cst328`. Add `espressif/esp_lcd_ili9341`, `espressif/esp_lcd_touch_tt21100`, `espressif/esp_codec_dev`, `espressif/button`. |
| `src/board.h` | **New.** Single source of truth for every BOX-3B pin. |
| `src/bus.c` / `bus.h` | **New.** Owns the shared `i2c_master_bus_handle_t` for touch + audio codec. |
| `src/display.c` | Rewrite: ST7789 → ILI9342C via `esp_lcd_ili9341`. Pins from `board.h`. Landscape via `swap_xy` + mirror. Public API (`display_init`, `display_panel`, `display_io`, `DISPLAY_WIDTH`, `DISPLAY_HEIGHT`) unchanged. |
| `src/display.h` | `DISPLAY_WIDTH=320`, `DISPLAY_HEIGHT=240`. |
| `src/touch.c` | Rewrite: CST328 → TT21100 via `esp_lcd_touch_tt21100`. I2C bus from `bus_i2c()`. Public API unchanged. |
| `src/voice.c` | Substantial rewrite: I2S-direct-to-DAC → `esp_codec_dev` over ES8311. PA enable line driven before first sample. Public API (`voice_say`, `voice_stop`, fade-in) unchanged. |
| `src/mic.c` / `mic.h` | **New.** ES7210 capture via `esp_codec_dev`. Exposes `mic_init`, `mic_read`, `mic_level`. |
| `src/imu.c` / `imu.h` | **Deleted.** All references removed from `app_main.c` and `src/CMakeLists.txt`. |
| `src/buttons.c` / `buttons.h` | **New.** Wraps `espressif/button` for BOOT (GPIO0); binds it to screen-back. Adds a tiny GPIO-input poll for the MUTE slide switch (GPIO1) that logs state transitions. |
| `src/devcfg.c` | Backlight pin constant updated; LEDC PWM logic unchanged. |
| `src/app_main.c` | New init order (see below). IMU stanza removed. `mic_init` and `buttons_init` added. |
| `src/{creature,wallet,settings,swap,wifi,info,config,menu}_screen.c` | All 8 re-laid-out for 320×240 landscape. Logic untouched, geometry rewritten. |
| `README.md` | Hardware section + pin table rewritten for BOX-3B. Build instructions otherwise identical. |

### Untouched (hardware-agnostic)

`wifi_sta.c`, `server.c`, `ai.c`, `x402.c`, `wallet.c`, `solana_tx.c`,
`base58.c`, `price.c`, `swap.c`, `testharness.c`, `ed25519/`. The whole
"logical app" rides over the new drivers without change.

### Managed components (new `idf_component.yml`)

```yaml
dependencies:
  idf: ">=5.1"
  lvgl/lvgl:                       "^9.2.0"
  espressif/esp_lvgl_port:         "^2.4.0"
  espressif/esp_lcd_touch:         "^1.1.2"
  espressif/esp_lcd_touch_tt21100: "^1.1.0"
  espressif/esp_lcd_ili9341:       "^2.0.0"
  espressif/esp_codec_dev:         "^1.3.0"
  espressif/button:                "^4.0.0"
```

Removed: `waveshare/esp_lcd_touch_cst328`.

### Init order (`src/app_main.c`)

```
nvs_init()
  → wifi_sta_init()                  # async, runs in background
  → bus_init()                       # shared I2C master bus, comes first
  → display_init()                   # must precede touch (shared RST line)
  → touch_init()
  → devcfg_init()                    # backlight up at stored duty
  → ui_init()                        # LVGL display + screen stack
  → screens_register_all()
  → voice_init()                     # ES8311 codec, PA enable
  → mic_init()                       # ES7210 codec, RMS-level task
  → buttons_init()                   # BOOT → back-pop, MUTE switch → log
  → server_init()                    # web UI
  → main loop (LVGL tick + say-worker)
```

### Shared-bus rationale

On BOX-3B the **TT21100 touch IC and the ES8311 audio codec live on the same
I2C bus** (`I2C_NUM_0`). Without `bus.c`, whichever driver inits second calls
`i2c_new_master_bus()` on an already-claimed port and gets
`ESP_ERR_INVALID_STATE`.

`bus.c` exposes:

```c
i2c_master_bus_handle_t bus_i2c(void);   // lazy-init on first call
```

`touch.c` and the codec setup in `voice.c` / `mic.c` both call this.
Touch INT and audio codec interrupts remain independent (their respective
`int_gpio_num` fields point at separate GPIOs).

### Shared LCD/touch reset rationale

On BOX-3B, **`LCD_RST` and `TOUCH_RST` are the same physical GPIO**, driven
through level shifters to both the panel and the touch IC. Implication:
`display_init()` must run *before* `touch_init()`. If touch initializes first,
`display_init()`'s panel-reset pulse will yank the touch IC out from under
the LVGL input device. The init order above enforces this; `board.h`
documents it next to the pin definition.

## Pin map (`src/board.h`)

Single header, no `.c`, grouped by subsystem. Every other source file
includes it; no hard-coded GPIO numbers anywhere else.

GPIOs are locked from `espressif/esp-bsp` `bsp/esp-box-3/include/bsp/esp-box-3.h`
(master, fetched 2026-04-27), the canonical Espressif source for this board.
That BSP also serves the BOX-3B variant — only the touch IC differs.

```c
// Display: ILI9342C over SPI3
#define BOARD_LCD_SPI_HOST          SPI3_HOST
#define BOARD_LCD_PIN_MOSI          GPIO_NUM_6
#define BOARD_LCD_PIN_SCLK          GPIO_NUM_7
#define BOARD_LCD_PIN_CS            GPIO_NUM_5
#define BOARD_LCD_PIN_DC            GPIO_NUM_4
#define BOARD_LCD_PIN_RST           GPIO_NUM_48   // shared with touch RST
#define BOARD_LCD_PIN_BL            GPIO_NUM_47   // owned by devcfg LEDC
#define BOARD_LCD_PIXEL_CLOCK_HZ    (40 * 1000 * 1000)
#define BOARD_LCD_H_RES             320
#define BOARD_LCD_V_RES             240

// Shared I2C bus (touch + ES8311 control)
#define BOARD_I2C_PORT              I2C_NUM_0
#define BOARD_I2C_PIN_SDA           GPIO_NUM_8
#define BOARD_I2C_PIN_SCL           GPIO_NUM_18
#define BOARD_I2C_HZ                400000

// Touch: TT21100 (BOX-3B)
#define BOARD_TOUCH_PIN_INT         GPIO_NUM_3
#define BOARD_TOUCH_PIN_RST         BOARD_LCD_PIN_RST  // shared via level-shifter

// Audio (I2S + shared I2C for codec control)
#define BOARD_I2S_PORT              I2S_NUM_0
#define BOARD_I2S_PIN_MCLK          GPIO_NUM_2
#define BOARD_I2S_PIN_BCLK          GPIO_NUM_17
#define BOARD_I2S_PIN_LRCK          GPIO_NUM_45
#define BOARD_I2S_PIN_DOUT          GPIO_NUM_15   // ESP -> ES8311
#define BOARD_I2S_PIN_DIN           GPIO_NUM_16   // ESP <- ES7210
#define BOARD_AUDIO_PIN_PA_EN       GPIO_NUM_46   // speaker amp enable
#define BOARD_ES8311_I2C_ADDR       0x18
#define BOARD_ES7210_I2C_ADDR       0x40

// User inputs
#define BOARD_BTN_PIN_BOOT          GPIO_NUM_0    // momentary, active-low
#define BOARD_MUTE_PIN              GPIO_NUM_1    // slide switch state, NOT a button
```

Two non-obvious facts the BSP confirmed and that the implementation must
respect:

- **LCD on SPI3, not SPI2.** The current Waveshare code uses `SPI2_HOST`;
  M1 must switch.
- **Touch reset has no dedicated GPIO.** It's level-shifted from `LCD_RST`,
  so `display_init()` must run before `touch_init()` or the panel reset
  yanks the touch IC mid-boot.

## LVGL screen re-layout

Every screen converts from 240×320 portrait to 320×240 landscape. Logic
stays; geometry rewrites.

### Generic pattern

24-px header strip on top (active screen title + status icons) + content
area (`216 × 320`) below. LVGL flex with row direction handles the
mechanical conversions; per-screen tweaks listed below.

### Per-screen plan

| Screen | Layout strategy | Notes |
|---|---|---|
| `creature_screen` | Sprite anchored center-left (`160 × 216` box). Subtitle box to its right (`~150 × 216`), multi-line wrap. | The "daemon looks at you" feel survives, with breathing room next to it. |
| `wallet_screen` | QR pinned left (`200 × 200`). Address + balance + buttons stack to its right. | Reads better in landscape than today. |
| `settings_screen` | Rows-of-toggles in a wider/shorter scroll list. | Mechanical. |
| `swap_screen` | Modal-style, centered. Internal flex stays vertical. | The CST328 → TT21100 release-debounce in `swap_screen.c:159` is re-tested on hardware; threshold may need tuning. |
| `wifi_screen` | On-screen keyboard gets full 320 width. | **Biggest user-facing win** — keyboard becomes phone-aspect-ratio. |
| `info_screen` | Two-column label / value layout instead of stacked rows. | Easier to scan. |
| `config_screen` | Same pattern as `settings_screen`. | Mechanical. |
| `menu_screen` | 2×2 grid of `160 × 120` tiles instead of 2×1 stack. | Touch targets stay finger-friendly. |

## Phasing

Four merge-able milestones. Each is a working build that flashes, taps,
demos, and is bisectable.

| # | Milestone | What works at the end | Estimate |
|---|---|---|---|
| 1 | **Boot + display + touch** | BOX-3B boots. `display.c` clears to navy. `ui_init()` runs and the existing production screens load (they look rotated/clipped because they're still in 240×320 portrait — that's expected and M2 fixes it). Tapping the panel produces correct landscape-coord events into LVGL's pointer indev. Init order verified: 5× power-cycle never breaks touch (shared LCD/touch reset hazard). | 1–2 days |
| 2 | **All 8 screens re-laid-out** | Whole app tappable end-to-end. Wi-Fi connects, web UI works, x402 chat completes (typed input only). `voice.c` exists but skips actual playback. "The whole app, silent." | 2–4 days |
| 3 | **Audio out + audio in** | TTS speaks via ES8311. `mic_init()` runs; 1 Hz RMS task logs to serial. No app consumer for mic. | 2–3 days |
| 4 | **User inputs** | BOOT button debounced and pops screen stack. MUTE switch transitions log to serial. | 0.5 day |

**Total: ~6–10 days of focused work.** Milestones 1 and 4 are mechanical;
2 (screens) and 3 (codec) are the time sinks.

Each milestone gets its own commit (or commits) on `espidf-skeleton`. No
separate branches needed since we are replacing, not coexisting.

## Verification per milestone

| Milestone | Verification |
|---|---|
| 1 | Visual: navy clear-screen during boot, then whichever production screen `ui_init()` lands on (likely creature_screen, rendered in its old portrait layout — visually wrong but proves LVGL is alive). Tap each corner; LVGL's pointer cursor follows your finger. Power-cycle 5×; confirm `TT21100 bound to LVGL` appears on every boot. |
| 2 | Tap through every screen. Wi-Fi onboarding from cold. Web UI loads at `http://<ip>/`. Type a chat message, confirm x402 round-trip lands a paid response. (No audio expected.) |
| 3 | Trigger a TTS reply; speaker plays clean audio. Tap the device while it speaks; confirm `voice_stop()` cuts cleanly. Cup mic with hand; serial RMS log drops; release; log rises. |
| 4 | Press BOOT from any screen — pops to previous. Slide MUTE between positions — each transition logs `mute=on` / `mute=off`. |

## Out of scope

- **Push-to-talk wiring** — mic → STT → chat input. Mic API is exposed; no consumer in this migration.
- **`esp-sr` wake-word** — needs a partition-table change (~3 MB model partition) when it lands. Confirmed direction; not in this branch.
- **MUTE switch binding to actual audio mute** — switch state is logged but doesn't gate the speaker yet.
- **Dual-board support** — Waveshare env is gone.
- **Touch-controller auto-detection** — committing to TT21100. A non-B BOX-3 (GT911) would need a manual driver swap, not runtime detection.
- **Partition layout changes** — 16 MB layout untouched; `esp-sr` will require revisiting in a follow-up.

## Risks

- **SPI host change.** Existing code targets SPI2; BOX-3 LCD is on SPI3. A
  missed reference to `SPI2_HOST` in any subsystem will silently route
  pixels to the wrong pins. Mitigation: a single grep-pass for `SPI2_HOST`
  in M1 and replace via `BOARD_LCD_SPI_HOST` from `board.h`.
- **Shared I2C bus init ordering.** Touch and audio both want `I2C_NUM_0`.
  Mitigation: `bus.c` lazy-init pattern. If a third subsystem ever needs the
  bus, it goes through the same handle.
- **Shared LCD/touch reset line.** Wrong init order silently kills touch.
  Mitigation: documented in `board.h` and enforced in `app_main.c` order.
  Verification step in milestone 1 explicitly power-cycles 5× to confirm.
- **TT21100 release jitter unlike CST328.** The debounce in
  `swap_screen.c:159` was tuned for CST328 dropouts; TT21100 has different
  failure modes. Mitigation: re-verify in milestone 2; tune threshold if
  needed.
- **Octal PSRAM sdkconfig.** BOX-3 uses Octal, Waveshare used Quad. Wrong
  config → boot loop. Mitigation: regenerate `sdkconfig.esp32_s3_box_3b`
  from scratch using `idf.py menuconfig` against `esp32-s3-devkitc-1`,
  selecting Octal explicitly.

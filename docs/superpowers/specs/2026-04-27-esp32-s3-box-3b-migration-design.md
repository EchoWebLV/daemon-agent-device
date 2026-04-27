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
| Buttons | none | 3 side buttons (PREV / NEXT / MUTE) |
| IR | none | front-panel IR receiver |

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
   - `BTN_PREV` → "back / pop screen" via existing screen stack.
   - `BTN_NEXT`, `BTN_MUTE` → log presses to serial only.
   - IR receiver → log decoded NEC codes to serial only.
   - Mic → 1 Hz RMS-level task logs to serial.

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
| `src/buttons.c` / `buttons.h` | **New.** Wraps `espressif/button`; binds `BTN_PREV` to screen-back. |
| `src/ir.c` / `ir.h` | **New.** RMT NEC decoder; logs `addr=0x%04x cmd=0x%02x` to serial. |
| `src/devcfg.c` | Backlight pin constant updated; LEDC PWM logic unchanged. |
| `src/app_main.c` | New init order (see below). IMU stanza removed. `mic_init`, `buttons_init`, `ir_init` added. |
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
  → buttons_init()                   # 3 buttons, BTN_PREV → back-pop
  → ir_init()                        # RMT NEC decoder
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

```c
// board.h shape (exact GPIOs locked from BOX-3B schematic v1.0 at impl)

// Display: ILI9342C over SPI2
#define BOARD_LCD_SPI_HOST          SPI2_HOST
#define BOARD_LCD_PIN_MOSI          /* schematic */
#define BOARD_LCD_PIN_SCLK          /* schematic */
#define BOARD_LCD_PIN_CS            /* schematic */
#define BOARD_LCD_PIN_DC            /* schematic */
#define BOARD_LCD_PIN_RST           /* shared with touch RST */
#define BOARD_LCD_PIN_BL            /* schematic, owned by devcfg LEDC */
#define BOARD_LCD_PIXEL_CLOCK_HZ    (40 * 1000 * 1000)
#define BOARD_LCD_H_RES             320
#define BOARD_LCD_V_RES             240

// Shared I2C bus (touch + ES8311 control)
#define BOARD_I2C_PORT              I2C_NUM_0
#define BOARD_I2C_PIN_SDA           /* schematic */
#define BOARD_I2C_PIN_SCL           /* schematic */
#define BOARD_I2C_HZ                400000

// Touch: TT21100
#define BOARD_TOUCH_PIN_INT         /* schematic */
#define BOARD_TOUCH_PIN_RST         BOARD_LCD_PIN_RST  // shared

// Audio
#define BOARD_I2S_PORT              I2S_NUM_0
#define BOARD_I2S_PIN_BCLK          /* schematic */
#define BOARD_I2S_PIN_LRCK          /* schematic */
#define BOARD_I2S_PIN_DOUT          /* schematic, ES8311 */
#define BOARD_I2S_PIN_DIN           /* schematic, ES7210 */
#define BOARD_AUDIO_PIN_PA_EN       /* schematic, speaker amp enable */
#define BOARD_ES8311_I2C_ADDR       0x18
#define BOARD_ES7210_I2C_ADDR       0x40

// Side buttons (active-low)
#define BOARD_BTN_PIN_PREV          /* schematic */
#define BOARD_BTN_PIN_NEXT          /* schematic */
#define BOARD_BTN_PIN_MUTE          /* schematic */
#define BOARD_BTN_ACTIVE_LEVEL      0

// IR receiver
#define BOARD_IR_PIN_RX             /* schematic */
#define BOARD_IR_RMT_RESOLUTION_HZ  1000000
```

Exact GPIO numbers are filled in as the very first task of milestone 1,
cross-referenced against:

- Espressif ESP32-S3-BOX-3 schematic v1.0
- `espressif/esp_bsp_box_3` BSP source (as a sanity check; we don't depend
  on the package, just consult it)

That first commit is intentionally tiny and self-contained so it can be
reviewed against the schematic in isolation.

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
| 1 | **Boot + display + touch** | BOX-3B boots. `display.c` clears to navy. LVGL up with a single test screen showing "tap me"; taps register at correct landscape coords. No screens registered yet. | 1–2 days |
| 2 | **All 8 screens re-laid-out** | Whole app tappable end-to-end. Wi-Fi connects, web UI works, x402 chat completes (typed input only). `voice.c` exists but skips actual playback. "The whole app, silent." | 2–4 days |
| 3 | **Audio out + audio in** | TTS speaks via ES8311. `mic_init()` runs; 1 Hz RMS task logs to serial. No app consumer for mic. | 2–3 days |
| 4 | **Buttons + IR** | 3 buttons debounced. `BTN_PREV` pops screen. NEC remote codes log to serial. | 1 day |

**Total: ~6–10 days of focused work.** Milestones 1 and 4 are mechanical;
2 (screens) and 3 (codec) are the time sinks.

Each milestone gets its own commit (or commits) on `espidf-skeleton`. No
separate branches needed since we are replacing, not coexisting.

## Verification per milestone

| Milestone | Verification |
|---|---|
| 1 | Visual: navy clear-screen, then "tap me" pill at center. Tap any corner; check coords logged match the corner. Verify shared-RST init order doesn't break touch by power-cycling 5× and confirming touch responds every boot. |
| 2 | Tap through every screen. Wi-Fi onboarding from cold. Web UI loads at `http://<ip>/`. Type a chat message, confirm x402 round-trip lands a paid response. (No audio expected.) |
| 3 | Trigger a TTS reply; speaker plays clean audio. Tap the device while it speaks; confirm `voice_stop()` cuts cleanly. Cup mic with hand; serial RMS log drops; release; log rises. |
| 4 | Press each button: `BTN_PREV` from any screen pops to previous; the other two log. Point any TV remote at the front panel; watch decoded codes scroll on serial. |

## Out of scope

- **Push-to-talk wiring** — mic → STT → chat input. Mic API is exposed; no consumer in this migration.
- **`esp-sr` wake-word** — needs a partition-table change (~3 MB model partition) when it lands. Confirmed direction; not in this branch.
- **IR-driven UX** — e.g. binding TV-remote codes to nav. Receiver alive; no consumer.
- **Mute-button binding** — `BTN_MUTE` logs but doesn't mute.
- **Dual-board support** — Waveshare env is gone.
- **Touch-controller auto-detection** — committing to TT21100. A non-B BOX-3 (GT911) would need a manual driver swap, not runtime detection.
- **Partition layout changes** — 16 MB layout untouched; `esp-sr` will require revisiting in a follow-up.

## Risks

- **Audio pinout uncertainty.** Different `esp-bsp` revisions have shown
  different I2S WS / backlight assignments for BOX-3. Mitigation: lock pins
  from the official BOX-3 v1.0 schematic in the very first impl task, with a
  single dedicated commit, and cross-check against the latest `esp_bsp_box_3`
  source.
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

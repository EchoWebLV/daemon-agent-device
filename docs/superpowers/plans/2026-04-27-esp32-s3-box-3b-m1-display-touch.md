# ESP32-S3-BOX-3B Migration — Milestone 1: Boot + Display + Touch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the existing Daemon firmware on an ESP32-S3-BOX-3B with the
ILI9342C panel showing a known clear-color and the TT21100 touch IC reporting
landscape-correct tap coordinates. No audio, no app screens — just bring-up.

**Architecture:** Drop-in driver swap on the existing single-file-per-subsystem
layout. Replace ST7789/SPI2 with ILI9342C/SPI3, replace CST328 with TT21100,
introduce `bus.c` to share the I2C master bus between touch (now) and the
ES8311/ES7210 codecs (M3). Centralize every BOX-3B GPIO in `board.h`. Disable
audio and chat init in `app_main.c` until M2/M3 wire them to the new hardware.

**Tech Stack:**
- ESP-IDF 5.1+ via PlatformIO `espressif32 @ ^6.8.0`
- `esp_lcd` + `espressif/esp_lcd_ili9341` (managed component, covers ILI9342C)
- `espressif/esp_lcd_touch_tt21100` (managed component)
- `lvgl @ ^9.2.0` + `espressif/esp_lvgl_port @ ^2.4.0`
- New-style ESP-IDF 5.x I2C master API (`i2c_master_bus_handle_t`)

**Spec:** [`docs/superpowers/specs/2026-04-27-esp32-s3-box-3b-migration-design.md`](../specs/2026-04-27-esp32-s3-box-3b-migration-design.md)

**Branch:** Stay on `espidf-skeleton`. The spec was committed there; this plan
continues on the same branch.

**Hardware:** ESP32-S3-BOX-3B at `/dev/cu.usbmodem101` (USB-Serial/JTAG, MAC
`B4:3A:45:0B:7A:C8`).

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `src/board.h` | **Create** | Single source of truth for every BOX-3B pin. Header-only. |
| `src/bus.h` | **Create** | Public interface for `bus_i2c()`. |
| `src/bus.c` | **Create** | Lazy-init owner of the shared `i2c_master_bus_handle_t` for I2C_NUM_0. |
| `src/display.c` | **Rewrite** | ILI9342C / SPI3 / landscape via `swap_xy` + `mirror`, pins from `board.h`. Public API (`display_init`, `display_panel`, `display_io`) unchanged. |
| `src/display.h` | **Modify** | `DISPLAY_WIDTH=320`, `DISPLAY_HEIGHT=240`. |
| `src/touch.c` | **Rewrite** | TT21100 via `esp_lcd_touch_tt21100`, sources I2C bus from `bus_i2c()`, landscape coord max. Public API unchanged. |
| `src/devcfg.c` | **Modify** | Backlight LEDC GPIO sourced from `BOARD_LCD_PIN_BL`. PWM logic unchanged. |
| `src/app_main.c` | **Modify** | Remove IMU init + `on_shake` + `imu.h` include. Comment out `voice_begin`, `ai_begin`, `server_set_say_handler`, the boot-greeting `ui_deliver_reply` (re-enabled in M2/M3). |
| `src/imu.c`, `src/imu.h` | **Delete** | No IMU on BOX-3B. |
| `src/CMakeLists.txt` | **Modify** | Add `bus.c` to SRCS, remove `imu.c`. Swap `esp_lcd_touch_cst328` → `esp_lcd_touch_tt21100` in REQUIRES, add `esp_lcd_ili9341`. |
| `src/idf_component.yml` | **Modify** | Drop `waveshare/esp_lcd_touch_cst328`. Add `espressif/esp_lcd_ili9341` and `espressif/esp_lcd_touch_tt21100`. |
| `platformio.ini` | **Modify** | Rename env `[env:waveshare_esp32s3_28]` → `[env:esp32_s3_box_3b]`. |
| `sdkconfig.waveshare_esp32s3_28` | **Delete** | Old per-env auto-cache. New `sdkconfig.esp32_s3_box_3b` regenerates on first build. |
| `sdkconfig.defaults` | **No change** | Already Octal PSRAM, 16 MB flash, USB-Serial/JTAG console. Verified. |
| `README.md` | **Modify** | Replace Waveshare hardware section + pin table with BOX-3B equivalent. |

`voice.c`, `mic.c`, `buttons.c`, all `*_screen.c` are out of scope for M1.

---

## Tasks

### Task 1: Create `src/board.h` (verified BOX-3B pin map)

**Files:**
- Create: `src/board.h`

- [ ] **Step 1.1: Write `src/board.h`**

```c
// ---------------------------------------------------------------------------
//  board.h — single source of truth for ESP32-S3-BOX-3B GPIO assignments.
//
//  Every other source file references these constants instead of hard-coding
//  GPIO numbers. Keeping pins here means a future board port is one file.
//
//  Pinout was lifted from Espressif's official BSP source on 2026-04-27:
//    espressif/esp-bsp · bsp/esp-box-3/include/bsp/esp-box-3.h (master)
//  The same BSP serves both BOX-3 and BOX-3B; only the touch IC differs.
//
//  Two non-obvious facts the implementation must respect:
//
//    1. LCD lives on SPI3, not SPI2. The Waveshare predecessor used SPI2,
//       so any leftover `SPI2_HOST` reference will silently route pixels
//       to the wrong pins.
//
//    2. The TT21100 touch IC's reset line has no dedicated GPIO — it's
//       level-shifted from LCD_RST (GPIO48). That means display_init()
//       MUST run before touch_init(); a panel reset pulse after the
//       touch IC is up will yank the touch controller out from under
//       the LVGL pointer indev.
// ---------------------------------------------------------------------------
#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"

// === Display: ILI9342C over SPI3 ============================================
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

// === Shared I2C bus (touch + ES8311/ES7210 codec control) ====================
#define BOARD_I2C_PORT              I2C_NUM_0
#define BOARD_I2C_PIN_SDA           GPIO_NUM_8
#define BOARD_I2C_PIN_SCL           GPIO_NUM_18
#define BOARD_I2C_HZ                400000

// === Touch: TT21100 (BOX-3B variant) ========================================
#define BOARD_TOUCH_PIN_INT         GPIO_NUM_3
#define BOARD_TOUCH_PIN_RST         BOARD_LCD_PIN_RST  // level-shifted, see header

// === Audio: ES8311 (out) + ES7210 (in) — defined for M3, unused in M1 =======
#define BOARD_I2S_PORT              I2S_NUM_0
#define BOARD_I2S_PIN_MCLK          GPIO_NUM_2
#define BOARD_I2S_PIN_BCLK          GPIO_NUM_17
#define BOARD_I2S_PIN_LRCK          GPIO_NUM_45
#define BOARD_I2S_PIN_DOUT          GPIO_NUM_15   // ESP -> ES8311
#define BOARD_I2S_PIN_DIN           GPIO_NUM_16   // ESP <- ES7210
#define BOARD_AUDIO_PIN_PA_EN       GPIO_NUM_46   // speaker amp enable
#define BOARD_ES8311_I2C_ADDR       (0x18)
#define BOARD_ES7210_I2C_ADDR       (0x40)

// === User inputs — defined for M4, unused in M1 ==============================
#define BOARD_BTN_PIN_BOOT          GPIO_NUM_0    // momentary, active-low
#define BOARD_MUTE_PIN              GPIO_NUM_1    // slide switch state
```

- [ ] **Step 1.2: Verify file syntax via build (no callers yet, but checks header parses)**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds (board.h is not yet `#include`d anywhere; this just confirms nothing else broke).

- [ ] **Step 1.3: Commit**

```bash
git add src/board.h
git commit -m "$(cat <<'EOF'
board: add BOARD_* pin map for ESP32-S3-BOX-3B

Single source of truth for every GPIO assignment. Sourced from
espressif/esp-bsp's bsp/esp-box-3 header (master @ 2026-04-27).
Documents the two BOX-3 quirks: LCD lives on SPI3 (not SPI2 like
the Waveshare predecessor), and touch reset is level-shifted from
LCD_RST so display_init must run before touch_init.

Not yet referenced — driver rewrites in subsequent tasks pull in
these constants.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Create `src/bus.{h,c}` (shared I2C master bus)

On BOX-3B, the TT21100 touch IC and the ES8311/ES7210 audio codecs all share
`I2C_NUM_0`. The first driver to call `i2c_new_master_bus()` claims the port;
the second gets `ESP_ERR_INVALID_STATE`. `bus.c` is a tiny lazy-init owner
that hands out the same `i2c_master_bus_handle_t` to whoever needs it.

**Files:**
- Create: `src/bus.h`
- Create: `src/bus.c`

- [ ] **Step 2.1: Write `src/bus.h`**

```c
// ---------------------------------------------------------------------------
//  bus.h — shared I2C master bus for the BOX-3B's touch IC + audio codec.
//
//  Why this exists: TT21100 (touch) and ES8311/ES7210 (audio codec control)
//  all live on I2C_NUM_0. The new-style ESP-IDF 5.x i2c_master API only lets
//  one caller create the bus; the second i2c_new_master_bus() returns
//  ESP_ERR_INVALID_STATE. So we lazy-create it once, on first call, and
//  hand the same handle out to everyone.
//
//  Pin + speed come from board.h (BOARD_I2C_PIN_SDA / SCL / BOARD_I2C_HZ).
// ---------------------------------------------------------------------------
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the shared bus handle, creating it on first call. Panics
// (ESP_ERROR_CHECK) on failure — the device is unusable without I2C, so
// there's no graceful fallback.
i2c_master_bus_handle_t bus_i2c(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2.2: Write `src/bus.c`**

```c
// ---------------------------------------------------------------------------
//  bus.c — see bus.h for the why.
// ---------------------------------------------------------------------------
#include "bus.h"
#include "board.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "bus";

static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t bus_i2c(void) {
    if (s_bus) return s_bus;

    const i2c_master_bus_config_t cfg = {
        .i2c_port                     = BOARD_I2C_PORT,
        .sda_io_num                   = BOARD_I2C_PIN_SDA,
        .scl_io_num                   = BOARD_I2C_PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    ESP_LOGI(TAG, "I2C%d up @ %d kHz (SDA=%d SCL=%d)",
             BOARD_I2C_PORT, BOARD_I2C_HZ / 1000,
             BOARD_I2C_PIN_SDA, BOARD_I2C_PIN_SCL);
    return s_bus;
}
```

- [ ] **Step 2.3: Add `bus.c` to `src/CMakeLists.txt` SRCS**

Modify `src/CMakeLists.txt` — add `"bus.c"` alphabetically between `"base58.c"` and `"config_screen.c"`:

```cmake
        "base58.c"
        "bus.c"
        "config_screen.c"
```

- [ ] **Step 2.4: Build to verify it compiles**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds. `bus.c` compiles into the binary even though no caller exists yet.

- [ ] **Step 2.5: Commit**

```bash
git add src/bus.h src/bus.c src/CMakeLists.txt
git commit -m "$(cat <<'EOF'
bus: add shared I2C master bus owner

Lazy-creates the I2C_NUM_0 master bus on first call to bus_i2c() and
hands the same handle to subsequent callers. Required by the BOX-3B
where touch (TT21100) and audio codec control (ES8311/ES7210) both
live on the same I2C bus.

Not yet referenced — touch.c and the M3 audio rewrites will plug into
this.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Add managed-component dependencies (ILI9341 + TT21100)

Add the new drivers via `idf_component.yml` and wire them into `CMakeLists.txt`'s
REQUIRES. The CST328 dep stays for now — it gets dropped in Task 5 once
`touch.c` no longer references it.

**Files:**
- Modify: `src/idf_component.yml`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 3.1: Update `src/idf_component.yml`**

Replace the dependencies block with:

```yaml
dependencies:
  idf: ">=5.1"
  lvgl/lvgl:                       "^9.2.0"
  espressif/esp_lvgl_port:         "^2.4.0"
  espressif/esp_lcd_touch:         "^1.1.2"
  # BOX-3B touch IC. Replaces the Waveshare CST328 driver.
  espressif/esp_lcd_touch_tt21100: "^1.1.0"
  # ILI9342C driver (covers ILI9341 + ILI9342). Replaces the built-in
  # ST7789 wiring used by the Waveshare panel.
  espressif/esp_lcd_ili9341:       "^2.0.0"
  # CST328 dep stays one more task — touch.c still includes its header.
  # Removed in Task 5 once touch.c is on TT21100.
  waveshare/esp_lcd_touch_cst328:  "^1.0.4"
```

- [ ] **Step 3.2: Update `src/CMakeLists.txt` REQUIRES**

In the REQUIRES list, replace the comment block at the bottom with:

```cmake
        # LCD panel drivers — ILI9342 (BOX-3B) over esp_lcd, plus the
        # generic touch abstraction and the TT21100 vendor driver.
        # CST328 stays one more task while touch.c still uses it.
        esp_lcd_ili9341
        esp_lcd_touch
        esp_lcd_touch_tt21100
        esp_lcd_touch_cst328
```

(Replaces the existing comment + REQUIRES lines that mention CST328 by itself.
Keep the rest of the REQUIRES block unchanged.)

- [ ] **Step 3.3: Build to fetch the new managed components**

Run: `pio run -e waveshare_esp32s3_28`

Expected: PIO downloads `espressif/esp_lcd_ili9341` and
`espressif/esp_lcd_touch_tt21100` into `managed_components/`, then build
succeeds. (Existing code still uses ST7789 + CST328, which both still link.)

- [ ] **Step 3.4: Commit**

```bash
git add src/idf_component.yml src/CMakeLists.txt
git commit -m "$(cat <<'EOF'
deps: add esp_lcd_ili9341 and esp_lcd_touch_tt21100

Pulls in the BOX-3B display + touch drivers. CST328 dep is kept one
more commit so the build stays green while touch.c is still on the
old driver — Task 5 of the M1 plan removes it.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Rewrite `src/display.c` for ILI9342C / SPI3 / landscape

**Files:**
- Modify: `src/display.h` (resolution macros)
- Modify: `src/display.c` (full body rewrite)

- [ ] **Step 4.1: Update `src/display.h` resolution**

Find the existing `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` defines (currently `240` /
`320`) and replace with:

```c
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240
```

- [ ] **Step 4.2: Rewrite `src/display.c`**

Replace the entire file with:

```c
// ---------------------------------------------------------------------------
//  display.c — ILI9342C bring-up for the ESP32-S3-BOX-3B.
//
//  Uses esp_lcd over SPI3 (BOX-3 routes the LCD bus to SPI3, NOT SPI2 like
//  the Waveshare predecessor). Driver comes from espressif/esp_lcd_ili9341,
//  which covers both ILI9341 and ILI9342C parts (the IC inside BOX-3 is a
//  9342C — same command set as 9341, slightly different gamma defaults; the
//  driver handles both transparently).
//
//  Orientation: panel is mounted as 320x240 landscape. We achieve that by
//  swapping x/y and mirroring x — same trick as the Arduino BSP.
//
//  Public API (display_init / display_panel / display_io / DISPLAY_WIDTH /
//  DISPLAY_HEIGHT) is unchanged so ui.c and the *_screen.c files compile
//  without modification.
//
//  Init order constraint: this MUST run before touch_init(). Touch RST is
//  level-shifted from LCD_RST (GPIO48); a late panel-reset pulse will yank
//  the touch IC out from under the LVGL pointer indev. board.h documents
//  the same.
// ---------------------------------------------------------------------------
#include "display.h"
#include "board.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;

esp_err_t display_init(void) {
    // ---- SPI bus (SPI3 on BOX-3) -------------------------------------------
    // max_transfer_sz sized so half a landscape frame fits one DMA burst:
    // 320 * 120 * 2 B (RGB565) = 76.8 KB. LVGL's two flush buffers are well
    // under this cap.
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BOARD_LCD_PIN_SCLK,
        .mosi_io_num     = BOARD_LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 120 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ---- Panel IO (SPI-mode wrapper around the bus) ------------------------
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BOARD_LCD_PIN_DC,
        .cs_gpio_num       = BOARD_LCD_PIN_CS,
        .pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io));

    // ---- ILI9342C panel ----------------------------------------------------
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,   // BOX-3 wires panel BGR
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // ILI9342 ships with colour inversion ON for typical mounting — same as
    // ST7789, the Arduino BSP, and the Waveshare predecessor.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    // Landscape: swap_xy + mirror x. The mirror combo we want was confirmed
    // empirically on the BOX-3; if your panel comes up flipped, try mirror(false, true)
    // or (true, true) — only one of the four combinations renders right-side-up.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ---- Clear to navy so we can tell "panel up" from "panel dark" ----------
    size_t   pixels = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    uint16_t *buf   = heap_caps_malloc(pixels * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        // ILI9342 expects RGB565 big-endian on the SPI wire; ESP32 is little-
        // endian, so byte-swap before fill. Without this, navy decodes as a
        // dim green — same gotcha as ST7789, see git log for the history.
        const uint16_t navy = __builtin_bswap16(0x0014);
        for (size_t i = 0; i < pixels; ++i) buf[i] = navy;
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
        free(buf);
    } else {
        ESP_LOGW(TAG, "no PSRAM for clear buffer; skipping");
    }

    ESP_LOGI(TAG, "ILI9342C %dx%d up @ %d MHz SPI%d",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             BOARD_LCD_PIXEL_CLOCK_HZ / 1000000,
             BOARD_LCD_SPI_HOST == SPI3_HOST ? 3 : 2);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void) {
    return s_panel;
}

esp_lcd_panel_io_handle_t display_io(void) {
    return s_io;
}
```

- [ ] **Step 4.3: Build to verify**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds. The new display.c links against `esp_lcd_ili9341`
which we added to REQUIRES in Task 3.

- [ ] **Step 4.4: Commit**

```bash
git add src/display.h src/display.c
git commit -m "$(cat <<'EOF'
display: rewrite for ILI9342C / SPI3 / landscape (BOX-3B)

ST7789 -> ILI9342C via espressif/esp_lcd_ili9341. Pins and SPI host
all sourced from board.h. Landscape orientation via swap_xy + mirror
so DISPLAY_WIDTH/HEIGHT report 320x240 to LVGL and the screens.

Public API unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Rewrite `src/touch.c` for TT21100 + drop CST328

**Files:**
- Modify: `src/touch.c` (full body rewrite)
- Modify: `src/idf_component.yml` (drop CST328)
- Modify: `src/CMakeLists.txt` (drop CST328 from REQUIRES)

- [ ] **Step 5.1: Rewrite `src/touch.c`**

Replace the entire file with:

```c
// ---------------------------------------------------------------------------
//  touch.c — TT21100 capacitive-touch bring-up for the ESP32-S3-BOX-3B.
//
//  Uses the espressif/esp_lcd_touch_tt21100 driver registered against the
//  generic esp_lcd_touch abstraction, then bound to LVGL via esp_lvgl_port
//  as a pointer indev. Same shape as the previous CST328 implementation —
//  only the IC and pin source change.
//
//  I2C bus comes from bus_i2c() (see bus.c) so we share I2C_NUM_0 with the
//  audio codec when M3 lands.
//
//  Reset line: TT21100 has no dedicated reset GPIO on the BOX-3B — its
//  reset is level-shifted from LCD_RST (GPIO48). board.h aliases
//  BOARD_TOUCH_PIN_RST to BOARD_LCD_PIN_RST for documentation, but
//  display_init() is the one that actually drives the line. As long as
//  display_init() runs first (it does, see app_main.c), the touch IC sees
//  exactly one reset pulse during boot.
// ---------------------------------------------------------------------------
#include "touch.h"
#include "display.h"
#include "board.h"
#include "bus.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t touch_init(void) {
    // ---- Panel IO for TT21100 over the shared I2C bus ----------------------
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_HZ;   // macro defaults to 100 kHz; we run 400 kHz.
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus_i2c(), &io_cfg, &tp_io),
        TAG, "tp io");

    // ---- TT21100 driver -----------------------------------------------------
    // Coordinate maxes match the panel's landscape orientation — display.c
    // configured the LCD as 320x240, so touch reports 0..319 X / 0..239 Y.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max          = DISPLAY_WIDTH,
        .y_max          = DISPLAY_HEIGHT,
        .rst_gpio_num   = -1,                    // no dedicated reset; see header
        .int_gpio_num   = BOARD_TOUCH_PIN_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_tt21100(tp_io, &tp_cfg, &s_touch),
        TAG, "tt21100 new");

    // ---- Bind to LVGL as a pointer input device ----------------------------
    const lvgl_port_touch_cfg_t lvgl_tp = {
        .disp   = lv_display_get_default(),
        .handle = s_touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&lvgl_tp), ESP_FAIL,
                        TAG, "lvgl_port_add_touch");

    ESP_LOGI(TAG, "TT21100 bound to LVGL (I2C %d kHz, INT=%d, RST=shared LCD_RST)",
             BOARD_I2C_HZ / 1000, BOARD_TOUCH_PIN_INT);
    return ESP_OK;
}
```

- [ ] **Step 5.2: Drop CST328 from `src/idf_component.yml`**

Remove the line:

```yaml
  waveshare/esp_lcd_touch_cst328:  "^1.0.4"
```

The dependencies block should now be:

```yaml
dependencies:
  idf: ">=5.1"
  lvgl/lvgl:                       "^9.2.0"
  espressif/esp_lvgl_port:         "^2.4.0"
  espressif/esp_lcd_touch:         "^1.1.2"
  espressif/esp_lcd_touch_tt21100: "^1.1.0"
  espressif/esp_lcd_ili9341:       "^2.0.0"
```

- [ ] **Step 5.3: Drop CST328 from `src/CMakeLists.txt` REQUIRES**

In REQUIRES, remove the `esp_lcd_touch_cst328` line. The block becomes:

```cmake
        esp_lcd_ili9341
        esp_lcd_touch
        esp_lcd_touch_tt21100
```

- [ ] **Step 5.4: Build to verify**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds. CST328 is no longer referenced anywhere; PIO will
prune `managed_components/waveshare__esp_lcd_touch_cst328` on next clean.

- [ ] **Step 5.5: Commit**

```bash
git add src/touch.c src/idf_component.yml src/CMakeLists.txt
git commit -m "$(cat <<'EOF'
touch: rewrite for TT21100 (BOX-3B) + drop CST328

Swaps the Waveshare CST328 driver for the official Espressif TT21100
driver and sources the I2C bus from bus_i2c() so the touch IC and
the M3 audio codec share I2C_NUM_0.

Touch reset line is level-shifted from LCD_RST on the BOX-3B, so we
pass rst_gpio_num=-1 — display_init() is the only thing that drives
the reset. This is also why display_init() must run before touch_init().

Public API unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Update `src/devcfg.c` to source backlight pin from `board.h`

The backlight LEDC PWM channel currently hard-codes the Waveshare GPIO5
backlight pin. Switch it to `BOARD_LCD_PIN_BL` (GPIO47 on BOX-3B). PWM
configuration, persistence, and the public API are unchanged.

**Files:**
- Modify: `src/devcfg.c`

- [ ] **Step 6.1: Update the backlight pin in `src/devcfg.c`**

The current file has at line 20:

```c
#define BL_PIN              5
```

and uses it at line 90 inside the LEDC channel config:

```c
.gpio_num   = BL_PIN,
```

Replace the `#define` line with an include + alias:

```c
#include "board.h"
// LCD backlight on the BOX-3B is GPIO47 — see board.h.
#define BL_PIN              BOARD_LCD_PIN_BL
```

Add `#include "board.h"` next to the existing includes near the top of the
file (around line 7-15) if not already present from the alias above. Update
the comment at line 17 to reference BOX-3B instead of Waveshare:

```c
// Pin matches src/board.h (GPIO47 on the BOX-3B). Owned exclusively by
// devcfg via LEDC PWM; no other subsystem touches the line.
```

The line-90 usage stays as `.gpio_num = BL_PIN,` — through the alias, it now
resolves to `BOARD_LCD_PIN_BL`.

- [ ] **Step 6.2: Build to verify**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds.

- [ ] **Step 6.3: Commit**

```bash
git add src/devcfg.c
git commit -m "$(cat <<'EOF'
devcfg: source backlight pin from board.h

PWM logic and persistence unchanged; just one pin constant moves.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Strip IMU and audio/chat init from `app_main.c`; delete `imu.c`/`imu.h`

For M1 we're verifying display + touch only. Everything else stays in the
source tree (untouched) but is not initialized at boot:

- IMU: doesn't exist on BOX-3B; deleted entirely.
- Voice (`voice_begin`): would grab GPIO 47 / 48 / 38 for I2S, conflicting
  with LCD_BL and LCD_RST. Must NOT run. M3 rewrites `voice.c` for the
  ES8311 codec.
- AI / chat (`ai_begin`, `server_set_say_handler`, the boot-greeting
  `ui_deliver_reply`): re-enabled in M2 once screens render correctly.

**Files:**
- Modify: `src/app_main.c`
- Modify: `src/CMakeLists.txt` (remove `imu.c` from SRCS)
- Delete: `src/imu.c`, `src/imu.h`

- [ ] **Step 7.1: Edit `src/app_main.c`**

In the `#include` block (around line 26), remove:

```c
#include "imu.h"
```

In the static helper functions section, delete the entire `on_shake()`
function (around lines 81–95) and its preceding comment block.

In `app_main()`:

1. Replace the `voice_begin()` block (lines 184–189) with:

```c
    // M1 (BOX-3B migration): voice path disabled. Original code grabbed
    // I2S on GPIO47/48/38 which conflict with LCD_BL and LCD_RST on the
    // BOX-3B. M3 rewrites voice.c around the ES8311 codec; until then
    // voice_begin() is intentionally not called.
```

2. Replace the `imu_begin()` block (lines 191–199) with:

```c
    // No IMU on BOX-3B — shake-to-talk is gone. Replaced in a future
    // branch by push-to-talk on the BOOT button (M4) once the mic
    // capture path lands in M3.
```

3. Replace the `ai_begin()` and `server_set_say_handler()` block
   (lines 175–182) with:

```c
    // M1: chat path disabled. ai_begin() is fine to call (no GPIO use)
    // but the resulting ui_deliver_reply() chain hits voice_speak()
    // through ui.c, which expects voice_begin() to have run. Restored
    // in M2 once the screens are working.
```

4. Replace the `ui_deliver_reply("Hello, I am Daemon.")` block
   (lines 242–244) with:

```c
    // M1: boot greeting disabled (would call voice_speak()). Restored
    // in M2.
```

- [ ] **Step 7.2: Remove `imu.c` from `src/CMakeLists.txt` SRCS**

Delete the line `"imu.c"` from the SRCS list.

- [ ] **Step 7.3: Delete `src/imu.c` and `src/imu.h`**

```bash
git rm src/imu.c src/imu.h
```

- [ ] **Step 7.4: Build to verify**

Run: `pio run -e waveshare_esp32s3_28`

Expected: build succeeds. No more references to `imu.h`. `app_main.c` no
longer instantiates audio or chat init.

- [ ] **Step 7.5: Commit**

```bash
git add src/app_main.c src/CMakeLists.txt
git commit -m "$(cat <<'EOF'
app: strip IMU + audio + chat init for BOX-3B M1

- imu.c/h deleted; BOX-3B has no IMU.
- voice_begin() commented out — would grab GPIO47/48 which conflict
  with LCD_BL and LCD_RST on the BOX-3B. M3 rewrites voice.c for the
  ES8311 codec.
- ai_begin / server_set_say_handler / boot-greeting commented out —
  the chat -> voice_speak chain expects voice_begin to have run.
  Restored in M2.

Display + touch + LVGL + Wi-Fi + web UI + wallet + price all still
init normally.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Switch PlatformIO env name to `esp32_s3_box_3b`

**Files:**
- Modify: `platformio.ini`
- Delete: `sdkconfig.waveshare_esp32s3_28`

- [ ] **Step 8.1: Rename the env in `platformio.ini`**

Change line 18 from:

```ini
[env:waveshare_esp32s3_28]
```

to:

```ini
[env:esp32_s3_box_3b]
```

Also update the file-header comment block (lines 1–16) so the description
matches:

```ini
; =============================================================================
; Daemon — ESP-IDF build for the Espressif ESP32-S3-BOX-3B.
;
; Migrated from the Waveshare ESP32-S3-Touch-LCD-2.8 in 2026-04. ESP-IDF only
; (no Arduino), so every subsystem speaks the same API: esp_http_client,
; esp_lcd, esp_wifi, lvgl, eventually esp-sr for wake-word. Pin assignments
; live in src/board.h, sourced from Espressif's official esp-bsp BOX-3 source.
;
; Layout:
;   platformio.ini         — this file
;   sdkconfig.defaults     — board-wide IDF config (Octal PSRAM, flash, CDC)
;   partitions.csv         — 16 MB layout: app + littlefs
;   src/CMakeLists.txt     — IDF "main" component declaration
;   src/board.h            — single source of truth for all GPIOs
;   src/app_main.c         — entry point
;   src/idf_component.yml  — managed-component deps (LVGL, esp_lvgl_port, …)
; =============================================================================
```

- [ ] **Step 8.2: Delete the old per-env sdkconfig cache**

```bash
git rm sdkconfig.waveshare_esp32s3_28
```

PIO regenerates `sdkconfig.esp32_s3_box_3b` from `sdkconfig.defaults` on the
next build.

- [ ] **Step 8.3: Build to confirm the new env**

Run: `pio run -e esp32_s3_box_3b`

Expected: PIO regenerates sdkconfig for the new env, builds the same binary.
A new `sdkconfig.esp32_s3_box_3b` file appears in the project root — leave
it untracked for now (it's a build artifact; treat the same way the
`sdkconfig.waveshare_esp32s3_28` was treated, which was tracked in git).

Actually — check: was the old sdkconfig tracked? Run:

```bash
git log --oneline -1 -- sdkconfig.waveshare_esp32s3_28
```

If it shows commits, it was tracked. Track the new one too (same pattern):

```bash
git add sdkconfig.esp32_s3_box_3b
```

- [ ] **Step 8.4: Commit**

```bash
git add platformio.ini sdkconfig.esp32_s3_box_3b
git commit -m "$(cat <<'EOF'
pio: switch env to esp32_s3_box_3b

Old sdkconfig.waveshare_esp32s3_28 deleted — PIO regenerates the
per-env sdkconfig from sdkconfig.defaults on first build (Octal PSRAM,
QIO @ 80 MHz flash, USB-Serial/JTAG console — all values are correct
for BOX-3B without changes).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Flash and verify display + touch on hardware

**Files:** None (verification only).

- [ ] **Step 9.1: Confirm device is enumerating**

```bash
ls /dev/cu.usbmodem*
```

Expected output: `/dev/cu.usbmodem101`. If missing, unplug/replug, hold BOOT
+ tap RESET to force download mode.

- [ ] **Step 9.2: Flash**

```bash
pio run -e esp32_s3_box_3b -t upload
```

Expected: upload succeeds, device resets, runs the new binary.

- [ ] **Step 9.3: Open serial monitor and verify boot logs**

```bash
pio device monitor -e esp32_s3_box_3b
```

Expected log lines (in order, within 2 s of reset):

```
I (xxx) daemon: == Daemon booting (ESP-IDF skeleton) ==
I (xxx) daemon: chip: esp32s3 ...
I (xxx) display: ILI9342C 320x240 up @ 40 MHz SPI3
I (xxx) bus: I2C0 up @ 400 kHz (SDA=8 SCL=18)
I (xxx) touch: TT21100 bound to LVGL (I2C 400 kHz, INT=3, RST=shared LCD_RST)
```

Disconnect the monitor with `Ctrl+]`.

If the display log doesn't show `SPI3` or shows wrong dims, stop and read
the relevant subsystem's source — pin map issue.

- [ ] **Step 9.4: Verify clear-color on the panel**

Look at the device. Expected: panel briefly shows a uniform navy color
during boot (before LVGL takes over with whichever default screen ui_init()
loads). The LVGL screens may render rotated / clipped / weird because they
were laid out for portrait — that's expected and gets fixed in M2.

If the panel is dark / black: backlight pin may be wrong; double-check
`devcfg.c` is using `BOARD_LCD_PIN_BL` and the constant is `GPIO_NUM_47`.

If the panel shows the wrong colors (e.g., red where navy should be):
likely the BGR/RGB ordering — flip `rgb_ele_order` between
`LCD_RGB_ELEMENT_ORDER_RGB` and `LCD_RGB_ELEMENT_ORDER_BGR` in `display.c`.

If the panel is rotated 90°/180° wrong: try the other
`esp_lcd_panel_mirror()` combination (the comment in display.c lists the
options). Edit, re-flash, re-verify.

- [ ] **Step 9.5: Verify touch coordinates land in landscape**

While `pio device monitor` is attached, tap the four corners of the panel
in turn. The TT21100 driver doesn't log per-touch by default; to see coords,
add a one-line diagnostic temporarily — open `src/touch.c` and add this
read-then-log helper just before `lvgl_port_add_touch()`:

```c
    // TEMP: log raw coords on every read so the M1 verification step can
    // confirm landscape orientation. Removed in step 9.7.
    #include "esp_timer.h"
    static void log_coords_cb(esp_lcd_touch_handle_t tp) {
        uint16_t x[1], y[1];
        uint8_t cnt = 0;
        if (esp_lcd_touch_read_data(tp) == ESP_OK &&
            esp_lcd_touch_get_coordinates(tp, x, y, NULL, &cnt, 1) && cnt) {
            ESP_LOGI("touch.coord", "x=%u y=%u", x[0], y[0]);
        }
    }
```

(For M1 verification it's simpler to just trust the LVGL pointer indev — tap
each corner of the panel and watch the indev's draws follow. If LVGL's
default cursor draws under your finger for taps in all four corners, touch
is wired right. The temp logger above is only needed if something looks
off. Skip if everything responds.)

Tap corners:
- Top-left → ESP-side X≈0, Y≈0
- Top-right → X≈319, Y≈0
- Bottom-left → X≈0, Y≈239
- Bottom-right → X≈319, Y≈239

If a corner reports the wrong axis (e.g. top-right gives X≈0 instead of
X≈319), the touch IC's `swap_xy` / `mirror_x` / `mirror_y` flags need
adjustment. Edit `src/touch.c` `tp_cfg.flags`, re-flash, re-verify.

- [ ] **Step 9.6: Power-cycle 5× to verify shared-RST stability**

This is the spec's explicit shared-LCD/touch-reset regression check:

```bash
# In the monitor, watch for "TT21100 bound" on each boot.
# Use the device's reset button or:
pio device monitor -e esp32_s3_box_3b   # then Ctrl+T Ctrl+R to reset
```

Reset 5 times. After each reset, confirm:

1. `ILI9342C 320x240 up` appears.
2. `TT21100 bound to LVGL` appears.
3. Tapping the panel still produces visible LVGL response.

If any boot is missing the touch line or touch becomes unresponsive after
some boots: display reset is happening AFTER touch init (regression in init
order). Verify `display_init()` precedes `touch_init()` in `app_main.c`.

- [ ] **Step 9.7: Remove temporary touch logger if added**

If you added the diagnostic helper in step 9.5, remove it now. `touch.c`
should be clean.

```bash
git diff src/touch.c   # confirm no temp diagnostic remains
```

- [ ] **Step 9.8: Commit verification proof (optional, for posterity)**

If you captured a serial log of a clean boot and touch test, save it under
`docs/superpowers/verification/2026-04-27-m1-flash-and-touch.txt` — handy
to point back to from the M2 plan. Skip if you didn't.

```bash
# only if you saved a log
git add docs/superpowers/verification/2026-04-27-m1-flash-and-touch.txt
git commit -m "verification: M1 boot + touch log capture"
```

---

### Task 10: Update `README.md` for BOX-3B

**Files:**
- Modify: `README.md`

- [ ] **Step 10.1: Replace the Hardware section**

Find the existing **Hardware** section (around line 35) and replace it with:

```markdown
## Hardware

Espressif **ESP32-S3-BOX-3B** — ILI9342C 320×240 LCD, TT21100 capacitive
touch, ES8311 audio codec + ES7210 mic codec (M3), 8 MB Octal PSRAM,
16 MB flash. Single USB-C for power + flash + serial via the chip's
native USB-Serial/JTAG.

Pin assignments live in [src/board.h](src/board.h), sourced from
Espressif's official BSP at
[`esp-bsp/bsp/esp-box-3`](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3).
Summary:

| Role | Pin |
|---|---|
| LCD MOSI | GPIO6 |
| LCD SCLK | GPIO7 |
| LCD CS | GPIO5 |
| LCD DC | GPIO4 |
| LCD RST | GPIO48 (shared with touch RST) |
| LCD BL | GPIO47 |
| LCD bus | SPI3 |
| Shared I2C SDA | GPIO8 |
| Shared I2C SCL | GPIO18 |
| Touch INT | GPIO3 |
| I2S BCLK | GPIO17 |
| I2S MCLK | GPIO2 |
| I2S LRCK | GPIO45 |
| I2S DOUT (→ ES8311) | GPIO15 |
| I2S DIN (← ES7210) | GPIO16 |
| Speaker amp enable | GPIO46 |
| BOOT button | GPIO0 |
| MUTE switch | GPIO1 |
```

- [ ] **Step 10.2: Update build-and-flash section**

Replace the `pio run -t upload` block with one that names the new env:

```bash
pio run -e esp32_s3_box_3b -t upload
pio device monitor -e esp32_s3_box_3b
```

- [ ] **Step 10.3: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
readme: rewrite hardware section for BOX-3B

Pin table now matches src/board.h. Build commands point at the new
esp32_s3_box_3b env.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## End of M1

At this point:

- Branch `espidf-skeleton` has 10 new commits making the BOX-3B silicon work.
- The device boots, the display shows the navy clear color, taps register at
  landscape coords, the shared-LCD/touch-reset hazard is verified safe.
- M2's "re-layout 8 LVGL screens for landscape" plan can now be written
  against ground-truth-on-hardware behaviour.

Recommended `git log --oneline` shape after M1:

```
README: rewrite hardware section for BOX-3B
pio: switch env to esp32_s3_box_3b
app: strip IMU + audio + chat init for BOX-3B M1
devcfg: source backlight pin from board.h
touch: rewrite for TT21100 (BOX-3B) + drop CST328
display: rewrite for ILI9342C / SPI3 / landscape (BOX-3B)
deps: add esp_lcd_ili9341 and esp_lcd_touch_tt21100
bus: add shared I2C master bus owner
board: add BOARD_* pin map for ESP32-S3-BOX-3B
docs: spec — fold in BOX-3 BSP-verified pinout and drop IR
docs: ESP32-S3-BOX-3B migration design
```

Each commit builds independently. Bisectable.

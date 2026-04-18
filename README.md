# Ball & Dots — ESP32-S3 touchscreen game

A small arcade game for the **Waveshare ESP32-S3-Touch-LCD-2.8**
(ST7789 240×320 display + CST328 5-point capacitive touch).

A cyan ball cruises across the screen at an angle. Four walls surround the
playfield. Touch anywhere to steer the ball toward that point; lift your
finger and the ball keeps heading in whatever direction you last turned it.
Scattered yellow dots respawn as you collect them — each one ticks the score
up and nudges the ball a bit faster. Kiss a wall and it's game over.

## Hardware

| Component | Pin |
|---|---|
| LCD MOSI | GPIO45 |
| LCD SCLK | GPIO40 |
| LCD CS | GPIO42 |
| LCD DC | GPIO41 |
| LCD RST | GPIO39 |
| LCD BL | GPIO5 |
| TP SDA | GPIO1 |
| TP SCL | GPIO3 |
| TP INT | GPIO4 |
| TP RST | GPIO2 |

## Build & flash (PlatformIO)

1. Install [PlatformIO](https://platformio.org/) — the easiest path is the
   VS Code extension. CLI works too.
2. From this folder:

   ```bash
   pio run -t upload
   pio device monitor
   ```

   PlatformIO will auto-install `TFT_eSPI`, `CSE_CST328`, and `CSE_Touch`.
3. If the board refuses to enter bootloader mode automatically: hold **BOOT**,
   tap **RESET**, release **BOOT**, and re-run upload.

## Controls

- **Touch & drag** anywhere on the screen — the ball steers smoothly toward
  your fingertip (rate-limited, so the direction can't flip instantly).
- **No touch** — the ball keeps its last heading.
- **After GAME OVER** — lift your finger, then tap to start a new run.

## Tuning

Feel of the game lives at the top of `src/main.cpp`:

```cpp
static constexpr float SPEED_INIT = 1.4f;   // starting speed
static constexpr float SPEED_GAIN = 0.06f;  // speed bump per dot
static constexpr float SPEED_CAP  = 3.8f;   // upper limit
static constexpr float TURN_RATE  = 0.16f;  // rad/frame — lower = wider arcs
static constexpr int   MAX_DOTS   = 4;      // dots on screen at once
```

If the touch axes feel inverted or swapped, tweak `touch.setRotation(...)`
in `setup()` (values `0..3`) or simply remap `p.x` / `p.y` in `readTouch()`.

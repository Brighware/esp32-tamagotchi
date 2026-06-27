# ⌚ TamaWatchy

A tiny **smartwatch** firmware for the **Waveshare ESP32-C6-Touch-LCD-1.47** with a
software-rendered **dolphin virtual pet** as its headline app.

The home screen is an **analog watchface**. Press and hold to open an **app
drawer**, then tap **TamaWatchy** to raise a dolphin — feed it, train it, keep its
tank clean, watch it level up, get fat, get ripped… or die of obesity if you spoil
it rotten.

Built with **ESP-IDF v5.4 + LVGL v8**. There are **no image assets** — every pixel
(clock hands, the dolphin, every animation) is drawn in code. Clock and pet state
survive power-offs via NVS.

---

## Hardware

**Waveshare ESP32-C6-Touch-LCD-1.47** — a 172×320 IPS touch display board:

| Part | Chip | Bus |
|------|------|-----|
| MCU | ESP32-C6 (RISC-V, Wi-Fi 6 / BLE) | — |
| Display | JD9853, 172×320, color-inverted | SPI2 @ 80 MHz |
| Touch | AXS5106L (addr 0x63) | I²C0 @ 400 kHz |

Verified pinout (from the Waveshare BSP):

| Function | GPIO | Function | GPIO |
|----------|------|----------|------|
| LCD MOSI | 2 | LCD SCLK | 1 |
| LCD MISO | 3 | LCD CS | 14 |
| LCD DC | 15 | LCD RST | 22 |
| LCD Backlight | 23 | — | — |
| Touch SDA | 18 | Touch SCL | 19 |
| Touch INT | 21 | Touch RST | 20 |

> ⚠️ This is **not** the ESP32-C6-Touch-LCD-1.69. That board uses an ST7789 +
> CST816S on different pins and is **not** compatible with this firmware.

---

## Watch shell & navigation

| Screen | What it shows | Gestures |
|--------|---------------|----------|
| **Watchface** (home) | Analog clock — moving hour/minute/second hands, tick marks, date | **Long-press → app drawer** |
| **App drawer** | The TamaWatchy app tile | **Tap** tile → launch · **Long-press** → back to watchface |
| **TamaWatchy** | The dolphin pet | **Long-press** background → back to watchface |

The clock runs off the ESP32 system time (the board has no RTC chip) and is
persisted to NVS, so it resumes on reboot. Set it from inside TamaWatchy
(the **Lv** chip → info panel → `H±` / `M±`); the watchface updates to match.

---

## TamaWatchy — the dolphin pet

### Care
Four stats decay in real time; the dolphin's face and animations reflect its mood
(hearts, Zzz, a fish to chase, grime, frowns):

| Stat | Restore with | Notes |
|------|--------------|-------|
| **Food** | **Feed** | fish-catch + chomp animation |
| **Fun** | **Play** | a porpoising leap with a bouncing beach ball; costs energy |
| **Rest** | **Sleep** | regenerates while asleep (tap **Wake** to get up) |
| **Wash** | **Wash** | scrubs the tank and clears poop |

### Progression
- **Levels & XP** — every care action grants XP; the **Lv** chip + a gold XP bar
  track it, with a level-up banner.
- **Skills** — four skills trained through use, each granting bonuses:
  **Acrobatics** (Play), **Appetite** (Feed), **Grooming** (Wash), **Stamina**
  (Sleep). Viewable as bars in the info panel.

### Body shape
- **Feed a lot → fat**: the belly visibly bulges rounder.
- **Play/train a lot → ripped**: the back and shoulders bulk up, the dorsal fin
  grows, and a muscle crease appears.
- They oppose each other and drift back to average over time — balance the two.

### Consequences
- **Poop** — every third feed drops a poop on the sea floor. It piles up with **no
  limit** and **fouls the water**: clear blue → murky brown → **near-black** as it
  stacks up. **Wash** clears it.
- **Death by obesity** — overfeeding an already-full dolphin packs on weight fast.
  Sustained very high weight builds *strain*; max it out and the dolphin **dies**,
  floating belly-up. A death screen offers **New pet** to start over.

### First launch
On the first TamaWatchy launch (empty save) an arcade-style **name picker** appears:
`<` / `>` cycle the letter, **Add** appends, **Del** backspaces, **OK** confirms.

---

## Build & flash

This is a ready-to-build ESP-IDF project (target `esp32c6`).

```bash
# 1. Activate ESP-IDF (adjust to your install path)
. ~/esp/esp-idf/export.sh

# 2. Build
idf.py set-target esp32c6   # first time only
idf.py build

# 3. Flash + monitor (find PORT: ls /dev/cu.usbmodem* on macOS, /dev/ttyACM* on Linux)
idf.py -p PORT flash monitor   # exit the monitor with Ctrl-]
```

The first build pulls managed components (LVGL, esp_lvgl_port, esp_lcd_touch) from
the ESP Component Registry, so it needs network access. The display (JD9853) and
touch (AXS5106L) drivers are vendored in-tree under `components/`.

To reset to a factory-fresh watch (default time, no pet):

```bash
idf.py -p PORT erase-flash && idf.py -p PORT flash
```

---

## Project layout

```
CMakeLists.txt                    top-level project
partitions.csv                    4 MB-safe table (3 MB app partition)
sdkconfig.defaults                esp32c6 target, LVGL fonts, color-swap, flash size
dependencies.lock                 pinned managed-component versions
main/
  main.c                          SPI + panel + touch + LVGL bring-up, starts the watch shell
  watch.c/.h                      watch shell: analog watchface, app drawer, navigation
  clock.c/.h                      shared software clock (system time + NVS persistence)
  tamagotchi.c/.h                 TamaWatchy app: game logic, stats, UI, NVS, timers
  dolphin.c/.h                    software-drawn animated dolphin (LVGL canvas)
components/esp_bsp/               trimmed Waveshare BSP: display (JD9853), touch, i2c, spi
components/esp_lcd_jd9853/        JD9853 display driver (vendored)
components/esp_lcd_touch_axs5106/ AXS5106L touch driver (vendored)
```

---

## How it works

- **Everything is drawn in code.** The dolphin is modelled as length "stations"
  (back / belly / countershading lines) and filled as three coloured **triangle
  strips** (dark dorsal cape → medium flank → pale belly). A 2D transform
  (translate + rotate + vertical flip) lets it bob, bank through the play leap,
  and float belly-up when dead. Only 3-point triangles are ever filled — LVGL's
  software polygon fill corrupts memory past ~16 vertices.
- **The watchface** uses LVGL line objects for the hands (recomputed each second)
  and the dial, so it needs no framebuffer of its own.
- **One screen, swapped content.** Watchface / drawer / app each build onto the
  active LVGL screen; transitions tear down the previous screen's timers and
  release the dolphin canvas before clearing it, so there are no dangling
  pointers or leaked timers.
- **Persistence** — the pet lives in NVS (namespace `pet`), the clock in NVS
  (namespace `watch`); both are reusable across reboots.

---

## Credits & licensing

- Display/touch drivers and BSP pin definitions are derived from Waveshare's
  official **ESP32-C6-Touch-LCD-1.47** demo. The vendored `esp_lcd_jd9853` and
  `esp_lcd_touch_axs5106` components carry Espressif's Apache-2.0 headers.
- Built on [ESP-IDF](https://github.com/espressif/esp-idf) and
  [LVGL](https://github.com/lvgl/lvgl).

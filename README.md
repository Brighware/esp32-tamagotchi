# ⌚ TamaWatchy — a smartwatch with a dolphin pet — ESP32-C6-Touch-LCD-1.47

A tiny **smartwatch** for the **Waveshare ESP32-C6-Touch-LCD-1.47** (172×320 IPS,
JD9853 display + AXS5106L capacitive touch). The home screen is an **analog
watchface**; **press and hold** anywhere to open an **app drawer**, then tap
**TamaWatchy** to launch a software-rendered dolphin virtual pet. Built with
**ESP-IDF + LVGL v8**, no image assets — everything (clock hands, dolphin, UI) is
drawn in code. Clock and pet state persist across power-offs via NVS.

## Watch shell & navigation

- **Watchface (home)** — an analog clock with moving hour/minute/second hands, a
  date readout, and a "hold for apps" hint. The time runs off the ESP32 system
  clock (no RTC chip on this board) and is persisted to NVS.
- **Long-press** anywhere on the watchface → **app drawer**.
- **App drawer** — tap the **TamaWatchy** tile to launch the pet; **long-press**
  to go back to the watchface.
- **Inside TamaWatchy** — **long-press** (on the background/dolphin, not a button)
  to exit back to the watchface. The pet keeps living between visits.

## First TamaWatchy launch — name your dolphin

On the very first run (empty NVS) an arcade-style name picker appears: use `<` / `>`
to cycle the current letter, **Add** to append it, **Del** to backspace, and **OK**
to confirm (empty name defaults to "Finn"). The name is saved and used everywhere.

## Gameplay

Four stats decay over real time:

| Stat | Meaning | Restore with |
|------|---------|--------------|
| **Food** | hunger | **Feed** (+fish, catch + chomp animation) |
| **Fun**  | happiness | **Play** (porpoising leap + beach ball, costs energy) |
| **Rest** | energy | **Sleep** (regenerates while asleep) |
| **Wash** | hygiene | **Wash** (back to 100%) |

The dolphin's face and animation reflect its mood: happy hearts when content,
Zzz while sleeping, a fish to chase while eating, grime when dirty, a frown when
neglected. Tap **Sleep** to nap (the button becomes **Wake**); feeding wakes it.

**Body shape** — the dolphin's build reacts to how you treat it:
- **Feed a lot → fat.** Each feed adds weight and the belly visibly bulges rounder
  ("… is looking chubby!").
- **Play/train a lot → ripped.** Each Play builds muscle: the back and shoulders
  bulk up, the dorsal fin grows taller, the flipper thickens and a muscle crease
  shows on the flank ("… is looking ripped!").
- They oppose each other — Play burns fat and builds muscle, Feed adds fat and
  softens muscle, and both ease back toward average over time. Balance the two.

**Poop** — every third feed the dolphin makes a mess: a poop drops to the sea
floor and stays there. Messes foul the water (hygiene drops faster) and keep the
dolphin grumpy until you tap **Wash**, which scrubs the tank clean and clears all
poop. There's **no limit** — poop piles up across rows the longer you ignore it,
and the water darkens with it: clear blue → murky brown → **near black** when it
really stacks up. Wash snaps it back to clear blue.

**Death by obesity** — overfeeding has consequences. Feeding an already-full
dolphin packs on extra weight fast ("… is dangerously obese!"). Sustained very
high weight builds *strain*; if it maxes out, the dolphin **dies of obesity** and
floats belly-up. A death screen shows its level and age with a **New pet** button
to start over. (Slim back down with **Play** before it's too late to avoid it.)

## Leveling, skills & clock

- **Level / XP** — every care action grants XP. A yellow XP bar sits under the
  header and the **Lv** chip (top-right) shows the current level; filling the bar
  levels up with a brief banner.
- **Skills** — four skills train through use and grant bonuses:
  **Acrobatics** (Play → more fun, cheaper energy), **Appetite** (Feed → more food),
  **Grooming** (Wash → more fun), **Stamina** (Sleep → faster energy regen).
- **Clock** — a live software clock shows `HH:MM` in the header. Tap the **Lv**
  chip to open the info panel: full date + time, XP progress, the four skill bars,
  and `H±` / `M±` buttons to set the clock. (The 1.47 board has no RTC chip, so the
  time runs off the ESP32 system clock and is persisted to NVS — it resumes on
  reboot but doesn't count time spent powered off.)

## Hardware pinout (verified from Waveshare BSP)

| Function | GPIO | Function | GPIO |
|----------|------|----------|------|
| LCD MOSI | 2 | LCD SCLK | 1 |
| LCD MISO | 3 | LCD CS | 14 |
| LCD DC | 15 | LCD RST | 22 |
| LCD Backlight | 23 | — | — |
| Touch I2C SDA | 18 | Touch I2C SCL | 19 |
| Touch INT | 21 | Touch RST | 20 |

Display: JD9853 on SPI2 @ 80 MHz, 172×320, color-inverted, column offset 34.
Touch: AXS5106L on I2C0 @ 400 kHz, address 0x63.

## Build & flash

This repo already contains an ESP-IDF v5.4 project. From the project root:

```bash
# 1. Activate ESP-IDF (adjust path to your install)
. ~/esp/esp-idf/export.sh

# 2. Target is already esp32c6; build
idf.py build

# 3. Flash + open the serial monitor (replace PORT, e.g. /dev/cu.usbmodem* on macOS)
idf.py -p PORT flash monitor
```

To find the port: `ls /dev/cu.usbmodem*` (macOS) or `ls /dev/ttyACM*` (Linux).
Exit the monitor with `Ctrl-]`.

First build pulls managed components (LVGL, esp_lvgl_port, esp_lcd_touch) from the
ESP Component Registry, so it needs network access. The display (JD9853) and touch
(AXS5106L) drivers are vendored in-tree under `components/`.

## Project layout

```
CMakeLists.txt                    top-level project
partitions.csv                    4MB-safe table (3MB app partition)
sdkconfig.defaults                esp32c6 target, LVGL fonts, color-swap, flash size
main/
  main.c                          SPI + panel + touch + LVGL bring-up, starts the watch shell
  watch.c/.h                      watch shell: analog watchface + app drawer + navigation
  clock.c/.h                      shared software clock (system time + NVS persistence)
  tamagotchi.c/.h                 TamaWatchy app: game logic, stats, UI, NVS, timers
  dolphin.c/.h                    software-drawn animated dolphin (LVGL canvas)
components/esp_bsp/               trimmed Waveshare BSP: display(JD9853), touch, i2c, spi
components/esp_lcd_jd9853/        JD9853 display driver (vendored from Waveshare demo)
components/esp_lcd_touch_axs5106/ AXS5106L touch driver (vendored from Waveshare demo)
```

## Notes

- Stat decay is tuned for a lively demo (noticeable within seconds–minutes), not
  realism. Adjust the modulo intervals in `logic_timer_cb()` in
  [main/tamagotchi.c](main/tamagotchi.c) to taste.
- The pet auto-saves every ~20 s and on every action. Delete the NVS partition
  (`idf.py erase-flash`) to start a fresh dolphin.
- Built against ESP-IDF v5.4.1 / LVGL 8.4.

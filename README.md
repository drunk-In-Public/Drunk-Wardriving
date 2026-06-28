# NM-CYD-C5 Wardriving Firmware

A focused, single-purpose wardriving firmware for the Rockbase NM-CYD-C5 (ESP32-C5).
Passive Wi-Fi AP scanning + GPS tagging + WiGLE-format CSV logging, with a clean
single-screen touch UI instead of the deep menu trees found in general-purpose
firmware like Bruce, ESP32 Marauder, or HaleHound.

**This firmware is intentionally scan-only.** It does not deauth, jam, inject, or
transmit anything beyond normal passive Wi-Fi scanning (which briefly puts the
radio in STA mode to read beacon frames — it never sends attack traffic).

## Credit / lineage

- Board pinout and the WiGLE CSV format/dedup approach are adapted from the
  [Bruce firmware](https://github.com/pr3y/Bruce) project's `nm-cyd-c5` board
  definition and `modules/gps/wardriving.cpp` (passive-scan logic only — no
  Bruce core code is included, since it's tightly coupled to Bruce's full menu
  system).
- GPS status concepts (fix/satellite/accuracy surfacing) were inspired by
  ESP32 Marauder's (https://github.com/justcallmekoko) `GpsInterface`.
- HaleHound-CYD's (https://github.com/JesseCHale/HaleHound-CYD)
   menu organization was used only as UX inspiration; no
  HaleHound code is included (its release only ships compiled binaries).
- I would like to personally thank the creators of Bruce and Marauder,
  without thir hard work and dedication none of this would be possible for me.
  I created this entire project wil Claude AI, I have zero coding experience.
  This is my first time creating a real repository.

## Hardware

| Component | Notes |
|---|---|
| Rockbase NM-CYD-C5 | ESP32-C5, ST7789 240x320 display, XPT2046 resistive touch |
| MicroSD card | FAT32, used for CSV logging |
| GPS module (NEO-6M / GT-U7 or similar) | UART, 9600 baud NMEA |

### Wiring (from Bruce's nm-cyd-c5 connections.md)

| Signal | Pin |
|---|---|
| SPI SCK | 6 |
| SPI MISO | 2 |
| SPI MOSI | 7 |
| TFT CS | 23 |
| TFT DC | 24 |
| TFT BL | 25 |
| Touch CS | 1 |
| SD CS | 10 |
| GPS RX (ESP32) | 4 |
| GPS TX (ESP32) | 5 |

Display, touch, and SD all share the same SPI bus with separate CS lines —
this matches the onboard wiring of the NM-CYD-C5, no extra wiring needed
beyond plugging in an SD card and a GPS module.

## Building

This is a [PlatformIO](https://platformio.org/) project.

```bash
# from this directory
pio run                  # build
pio run -t upload        # build + flash
pio device monitor        # serial output for debugging
```

Dependencies (TFT_eSPI, TinyGPSPlus, XPT2046_Touchscreen) are declared in
`platformio.ini` and will be fetched automatically on first build.

## Touch calibration

The touch-to-screen mapping in `main.cpp` (`TOUCH_MIN_X/MAX_X/MIN_Y/MAX_Y`) uses
reasonable defaults for an ST7789/XPT2046 panel, but resistive touch panels
vary unit to unit. If tapping doesn't land where expected, run a basic
calibration sketch (XPT2046_Touchscreen examples include one) and update those
four constants.

## Alert watchlist

On first boot (or first SD card use), the firmware creates `/wardrive/alert.txt`
with a commented example. Add one MAC/BLE address per line (case-insensitive,
`#` for comments) to get an on-screen alert whenever that device is seen during
a session:

```
# Alert MAC/BLE addresses to watch for, one per line.
aa:bb:cc:dd:ee:ff
11:22:33:44:55:66
```

When a watched address appears, a banner shows briefly over the AP list and
the status bar's alert counter increments. The list is unaffected by which
file/session is active — edit `alert.txt` any time between sessions (it's
loaded once at startup).

## Using it

1. Power on with an SD card and GPS module connected.
2. Wait for "GPS: FIX" in the status bar (cold fix can take 30s–2min outdoors).
3. Tap **START**. A new file `wardrive/<timestamp>_wardrive.csv` is created on
   the SD card in WiGLE format, and the AP list begins populating, sorted by
   signal strength across Wi-Fi and BLE together.
4. Tap **STOP** to end the session. The CSV stays on the SD card — import it
   directly at [wigle.net](https://wigle.net) or open it in any spreadsheet
   tool / GIS software that reads WiGLE CSVs.

## Project layout

```
platformio.ini          - build config, board target, library deps
partitions_8mb.csv       - flash partition table
src/
  pins.h                 - board pin map
  WardrivingEngine.h/.cpp- scan + GPS + CSV logging logic (no UI dependency)
  UiDashboard.h/.cpp     - touch UI rendering (no scanning logic — reads engine state)
  main.cpp               - wiring/setup/loop
```

`WardrivingEngine` and `UiDashboard` are deliberately decoupled — the engine
has no display code, and the UI only reads engine state. That makes it
straightforward to swap in a different display library or add features (e.g.
BLE scanning, an alert-MAC watchlist) later without touching the other half.

## Known limitations / next steps

- No on-device map — this firmware logs coordinates but doesn't render a map.
  Reviewing the CSV in WiGLE or QGIS after the fact is the intended workflow.
- BLE scanning is now included alongside Wi-Fi (toggle via
  `engine.setBleEnabled(false)` in `main.cpp` if you only want Wi-Fi). Each
  scan cycle does a Wi-Fi sweep followed by a ~3s BLE window; both log to the
  same WiGLE-format CSV with `Type` set to `WIFI` or `BLE`. The on-screen list
  tags each row `[W]`/`[B]` and mixes both, sorted by signal strength.
- No SSID/AP de-duplication across sessions (only within a single session) —
  add a persistent BSSID/BLE-address cache file on SD if you want
  cross-session dedup.
- This hasn't been build-tested on real hardware in this environment (no
  network access to fetch the ESP-IDF toolchain/libraries) — please flag any
  build errors and I'll help debug them.

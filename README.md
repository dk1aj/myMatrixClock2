# myMatrixClock2

`myMatrixClock2` is the Teensy-side firmware for a 32x32 HUB75 matrix clock.
It renders local time, UTC, date, DST status, and accepts RTC updates from the
companion ESP32 NTP bridge over a small custom SPI protocol.

## Features

- 32x32 SmartMatrix clock display on Teensy 3.1 / 3.2
- local time from a DS1307 RTC
- UTC display with trailing `Z`
- DST status line shown as `CET` / `CEST`
- minute-level RTC updates received from the ESP32 companion project
- USB serial input for manual RTC setting
- Python GUI for serial monitoring and SPI regression tests

## Repository Layout

- `src/main.cpp`
  Teensy firmware for rendering, RTC access, USB input, and SPI slave receive.
- `python/SetCloch_GUI.py`
  PySide6 desktop tool for serial monitoring, time sending, and SPI test runs.
- `platformio.ini`
  PlatformIO environment for `teensy31`.
- `lib/SmartMatrix`
  Vendored SmartMatrix dependency used by the display firmware.

## Hardware

- Teensy 3.1 / 3.2
- 32x32 HUB75 RGB matrix compatible with SmartMatrix
- DS1307 RTC module
- companion ESP32 running the `NTP_2` project

### RTC Wiring

The firmware expects the RTC on the Teensy's alternate I2C pins:

- RTC `SCL` -> Teensy `16`
- RTC `SDA` -> Teensy `17`
- RTC `VCC` / `GND` as required by the module

### ESP32 SPI Link

The ESP32 sends ASCII timestamps in a 32-byte frame. Current Teensy-side
signal assignment:

- `CS`   -> Teensy `15`
- `SIN`  -> Teensy `11`
- `SOUT` -> Teensy `12`
- `CLK`  -> Teensy `13`
- common `GND`

## Display Layout

The current matrix layout is:

- large local time on the top row
- UTC line in red with a colored trailing `Z`
- centered date line
- centered `CET` / `CEST` status line
- blinking pixel in the bottom-right corner

## Manual RTC Input

The Teensy accepts manual USB serial input in this format:

```text
YYYY-MM-DD HH:MM:SS
```

Example:

```text
2026-04-06 17:30:00
```

## Python GUI

The GUI in `python/SetCloch_GUI.py` is intended for:

- opening Teensy and ESP32 serial ports
- monitoring RTC and NTP traffic
- sending manual time strings
- running the SPI regression suite

The GUI requires:

- Python 3
- `PySide6`
- `pyserial`

## Build

From the repository root:

```bash
pio run -e teensy31
```

The current `platformio.ini` contains local `upload_port` and `monitor_port`
settings (`COM7`). Adjust them to match your system before flashing.

## Notes For GitHub

- build products and Python cache files are ignored
- the repository contains local development notes in `my_plan.md`
- SmartMatrix remains vendored under `lib/SmartMatrix`

If you want a public repository to show an explicit license on GitHub, add a
top-level `LICENSE` file before publishing.

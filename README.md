# myMatrixClock2

## Purpose

`myMatrixClock2` is the Teensy firmware for a 32x32 HUB75 matrix clock. It
drives the display through SmartMatrix, stores local civil time in a DS1307
RTC, and receives time updates from an ESP32 running `NTP_2`.

The clock continues from the RTC when the ESP32 or NTP is unavailable.

## Hardware

- Teensy 3.1 / 3.2
- DS1307 RTC
- 32x32 HUB75 RGB matrix
- SmartMatrix

RTC connections:

| Signal | Teensy pin |
| --- | ---: |
| SCL | 16 |
| SDA | 17 |

ESP32 communication:

| Signal | Teensy pin |
| --- | ---: |
| CS | 15 |
| MOSI | 11 |
| MISO | 12 |
| CLK | 13 |

## Display

The matrix shows:

- local time
- UTC time
- date
- CET or CEST state
- NTP status pixel
- normal activity/blink pixel
- a visible red `RTC` message if reading the RTC fails

NTP status pixel:

- green: the current minute was successfully synchronized by NTP
- red: NTP failed and ESP32 system time was used
- off: no valid NTP status has been received yet

The RTC can also be set over USB serial with
`YYYY-MM-DD HH:MM:SS`.

## Time protocol

The Teensy receives the same fixed 32-byte frame documented by `../NTP_2`.
The important status fields are:

```text
Byte 20 = NTP status
Byte 21 = Sequence ID
Byte 22 = CET/CEST status
```

## Reliability

- An ACK is valid only when its Sequence ID matches the transmitted frame.
- Stale ACKs are rejected by the ESP32.
- The repeated autumn `02:xx` hour uses the explicit ESP32 timezone status.
- A stuck LOW CS line cannot continuously block the Teensy loop.
- RTC read failures replace stale clock data with a visible error message.

## Build

- PlatformIO environment: `teensy31`
- Main source: `src/teensy_main.cpp`
- Display library: SmartMatrix
- Current upload and monitor port: `COM19`

```bash
pio run -e teensy31
pio run -e teensy31 -t upload
```

## Related project

The ESP32 NTP sender is in `../NTP_2`.

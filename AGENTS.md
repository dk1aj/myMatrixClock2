# AGENTS.md

# Project overview

This workspace contains two related embedded projects that together form one clock system.

## Projects

### `myMatrixClock2`

Target:

- Teensy 3.1 / 3.2
- Arduino / Teensy framework
- PlatformIO

Purpose:

- Drives a 32×32 HUB75 RGB matrix using SmartMatrix.
- Uses a DS1307 RTC.
- Displays local time, date, UTC information and CET/CEST state.
- Receives time updates from the ESP32 project.
- Continues running from the RTC if the ESP32 is unavailable.

Main source file:

```text
myMatrixClock2/src/teensy_main.cpp
```

The active display implementation uses:

```text
SmartMatrix
```

It does NOT use FastLED directly.

Do not replace SmartMatrix with FastLED unless explicitly requested.

---

### `NTP_2`

Target:

- ESP32
- Arduino framework
- PlatformIO

Purpose:

- Connects to Wi-Fi.
- Obtains time using NTP.
- Applies the configured CET/CEST timezone.
- Sends local time to the Teensy.
- Uses the ESP32 system clock between NTP synchronizations.

Main source file:

```text
NTP_2/src/ntp_2.cpp
```

---

# System architecture

The normal time path is:

```text
NTP
 |
 v
ESP32
 |
 | local timestamp
 | ESP32 -> Teensy communication
 v
Teensy
 |
 v
DS1307 RTC
 |
 v
SmartMatrix display
```

The ESP32 provides external network time.

The Teensy stores accepted time in the DS1307.

The Teensy display reads the RTC and must remain usable when the ESP32 is unavailable.

---

# General working rules

Follow these rules for every task.

1. Preserve working code whenever possible.
2. Make the smallest change required to solve the requested problem.
3. Do not rewrite working modules merely for style.
4. Do not introduce new features unless explicitly requested.
5. Do not change hardware assumptions without explicit approval.
6. Do not change GPIO assignments unless explicitly requested.
7. Do not change communication formats silently.
8. Do not change library versions unless necessary and explicitly justified.
9. Prefer simple embedded solutions over abstractions.
10. Avoid unnecessary dynamic memory allocation.
11. Avoid blocking code where it affects communication or display operation.
12. Keep diagnostic output useful and concise.

Do not perform unrelated cleanup while implementing a requested change.

---

# Cross-project rule

The Teensy and ESP32 projects form one system.

Whenever a change affects communication between them, inspect BOTH projects before modifying code.

This includes changes involving:

- SPI
- message formats
- timestamps
- acknowledgements
- status codes
- timing
- retries
- timeouts
- sequencing
- timezone handling
- RTC updates

Never modify only one side of a protocol unless it has been verified that the other side remains compatible.

---

# Hardware protection

Existing GPIO assignments are considered fixed hardware interfaces.

Do not change GPIO assignments without explicit approval.

Current communication assignments include:

## Teensy

```text
RTC SCL     Pin 16
RTC SDA     Pin 17

SPI CS      Pin 15
SPI MOSI    Pin 11
SPI MISO    Pin 12
SPI CLK     Pin 13
```

SmartMatrix HUB75 pins are defined by the existing SmartMatrix hardware configuration and must not be reassigned casually.

## ESP32

```text
SPI CS      GPIO 5
SPI MOSI    GPIO 23
SPI MISO    GPIO 19
SPI CLK     GPIO 18
```

Treat these connections as existing physical wiring.

---

# RTC rules

The Teensy uses a DS1307 RTC.

Do not:

- replace the RTC library,
- change RTC pins,
- change the stored time representation,
- add automatic RTC corrections,
- or change synchronization frequency

unless the task specifically requires it.

When modifying RTC-related code, check:

- valid calendar ranges,
- RTC read failures,
- RTC write failures,
- reset behaviour,
- ESP32 failure behaviour,
- power-loss behaviour,
- timezone consequences.

The RTC currently stores local civil time.

Be particularly careful around the end of daylight saving time because the local hour from 02:00 to 02:59 occurs twice.

---

# Timezone rules

The ESP32 currently uses Central European time with daylight saving time.

Timezone configuration:

```text
CET-1CEST,M3.5.0/2,M10.5.0/3
```

Do not independently add another CET/CEST offset on the Teensy without analysing the entire time path.

Always distinguish between:

- UTC
- ESP32 system time
- local CET/CEST time
- DS1307 stored time
- displayed local time
- displayed UTC time

Do not assume these are interchangeable.

---

# Communication protocol

The current controller link is a software-implemented SPI-like protocol.

Current properties include:

```text
Master:          ESP32
Slave:           Teensy
Bit order:       MSB first
Clock idle:      LOW
Sampling edge:   rising
Equivalent mode: SPI Mode 0
Frame size:      32 bytes
```

The current time frame contains:

```text
Byte 0..18  YYYY-MM-DD HH:MM:SS
Byte 19     null terminator
Byte 20     current-minute NTP status
Byte 21     sequence ID
Byte 22..31 zero-filled
```

A separate `STATUS?` request obtains the Teensy result. The response returns
the status code in byte 0 and the related sequence ID in byte 1. The ESP32
accepts a successful response only when its sequence matches the transmitted
time frame.

Existing status values include:

```text
0x00  idle
0x01  accepted
0x02  parse/format error
0x03  RTC write error
```

Do not change this protocol silently.

Any protocol change must include:

1. Teensy analysis
2. ESP32 analysis
3. compatibility check
4. build of both projects
5. explanation of the new behaviour

---

# Known issues

The following known issues are NOT permission to modify them automatically.

They should only be addressed when explicitly requested.

## DST end

The DS1307 stores local civil time without timezone or DST state.

At the autumn CET/CEST transition the local `02:xx` hour occurs twice.

The current Teensy logic cannot uniquely distinguish both occurrences.

Do not apply a superficial one-line DST fix.

Analyse the complete UTC/local/RTC path before changing this.

## Software SPI

The communication is deliberately implemented in software and is relatively slow.

Do not replace it with hardware SPI solely as a cleanup or optimization.

---

# Removed code

A former ESP32-S3/LVGL demonstration program was removed from `myMatrixClock2`.

It is NOT part of the system.

Do not recreate or restore:

```text
dis08070h_main.cpp
dis08070h_backend.h
LGFX_ESP32S3_RGB_MakerfabsParallelTFTwithTouch70.h
lv_conf.h
lvgl/lvgl.h
elecrow_dis08070h_v3.json
```

Do not add LVGL or ESP32-S3 dependencies to `myMatrixClock2`.

---

# Source file rules

The active Teensy main source is:

```text
myMatrixClock2/src/teensy_main.cpp
```

Do not restore the obsolete:

```text
myMatrixClock2/src/main.cpp
```

unless explicitly requested.

The active ESP32 source is:

```text
NTP_2/src/ntp_2.cpp
```

---

# Build requirements

After any code modification, compile the affected project.

After every firmware-affecting code or configuration change, also flash the
affected device after a successful build. A coding task is not complete until
the required upload succeeds.

For communication, RTC or timestamp changes, compile and flash BOTH projects,
one after the other.

Expected PlatformIO environments:

```text
myMatrixClock2:
env:teensy31

NTP_2:
env:esp32dev
```

Current upload ports:

```text
myMatrixClock2 / Teensy: COM19
NTP_2 / ESP32:          COM16
```

Flash the Teensy first and the ESP32 second when both devices are affected.
Documentation-only changes do not require a firmware build or upload.

A task is not considered complete if required builds or uploads fail.

Report:

- build result,
- upload result,
- relevant warnings,
- RAM use,
- Flash use.

Do not hide compiler warnings introduced by the change.

---

# Testing

When modifying parsing, timestamps, RTC handling or communication, consider at least:

- normal valid timestamp
- malformed timestamp
- incomplete frame
- RTC write failure
- communication timeout
- ESP32 restart
- Teensy restart
- missing ESP32
- missing network
- minute transition
- day transition
- month transition
- year transition
- leap year
- CET -> CEST transition
- CEST -> CET transition

Do not claim hardware behaviour was verified if only compilation or static analysis was performed.

Clearly distinguish:

```text
static analysis
build verification
software test
hardware test
```

---

# Git rules

Before modifying files:

```bash
git status
```

After modifying files:

```bash
git status
```

Do not automatically:

- commit
- push
- create tags
- archive repositories

unless explicitly requested.

Do not stage unrelated files.

Pay particular attention to:

```text
.vscode/tasks.json
```

It must not be added automatically merely because it exists.

---

# Change reporting

For every completed coding task report:

## Before

Describe the relevant existing behaviour.

## Changes

List every modified, added or deleted file.

Explain the functional change in plain language.

## After

Describe the resulting behaviour.

## Build

Report build result for every affected project.

## Remaining issues

Mention known relevant problems that were deliberately left unchanged.

---

# Scope discipline

If the user requests one specific fix, implement that fix only.

Example:

If the task is:

```text
Fix SPI acknowledgement handling.
```

Do not also change:

- DST handling
- NTP synchronization
- display formatting
- RTC architecture
- SPI speed
- source layout

unless one of those changes is strictly required for the requested fix.

If an additional issue is discovered, report it instead of silently fixing it.

---

# Final rule

Analyse first.

Preserve working behaviour.

Make the smallest technically correct change.

Build and verify after changes.

Never invent hardware details.

When unsure about a hardware-related assumption, stop before changing it and report the uncertainty.

# Plan for Transferring NTP Time from `NTP_2` to `myMatrixClock2` via UART

## Goal

The time received by the ESP32 project `NTP_2` via Wi-Fi/NTP should be transferred to the Teensy project `myMatrixClock2`, so the RTC can be set there and the matrix clock can continue running independently afterward.

## Why Not SPI

In the current `myMatrixClock2` setup, there are pin conflicts caused by SmartMatrix.

Especially relevant:

- SmartMatrix already occupies several pins that would otherwise be attractive for SPI
- `Pin 13` is also used in the project as an LED pin
- the previously suggested pins `6`, `7`, and `8` are already occupied in the Teensy project

Because of that, SPI is unnecessarily complicated in this specific setup.

## Recommended Solution

The practical solution is:

1. The ESP32 remains the NTP client.
2. The ESP32 sends the time as a text line over a hardware UART.
3. The Teensy receives that time on `Serial1`.
4. USB serial remains separate for debugging and manual input.
5. The Teensy sets its RTC from the received time.

## Important Point About UART and USB

On the Teensy:

- `Serial` = USB serial
- `Serial1` = real hardware UART on pins

That means:

- debug output can continue over USB
- time transfer can happen in parallel over `Serial1`

So there is **no conflict** between UART and USB if `Serial1` is used.

## Recommended Pin Assignment

### Teensy 3.1

- `Serial1 RX` = `Pin 0`
- `Serial1 TX` = `Pin 1`

### Minimal Wiring

- ESP32 `TX` -> Teensy `Pin 0` (`RX1`)
- common `GND`

### Optional Return Path

- Teensy `Pin 1` (`TX1`) -> ESP32 `RX`

Recommendation for the first version:

- start with one-way communication only:
  - ESP32 sends
  - Teensy receives

## Why UART Fits Well Here

- very little additional code
- robust and easy to debug
- no SmartMatrix pin conflict
- the existing parsing logic in `myMatrixClock2` can be reused almost directly
- ideal for a small amount of data such as date/time

## Current Project Status

### `NTP_2`

- runs on ESP32
- receives time via Wi-Fi/NTP
- already has compact output:

```text
TIME_TX: 2026-03-28 18:12:34 | SRC: fritz.box
TIME_INFO: Y=2026 MO=03 D=28 WD=6 H=18 MIN=12 S=34 DST=1
```

### `myMatrixClock2`

- runs on Teensy 3.1
- uses `DS1307RTC`
- can already process time strings in the format

```text
YYYY-MM-DD HH:MM:SS
```

Important existing building blocks:

- `parseRtcInput()`
- `setRTCFromSerial()`
- `RTC.write(normalized)`
- `setTime(parsedTime)`

## Very Important Time-Base Note

`myMatrixClock2` currently treats the RTC as **local standard time (CET, UTC+1 without daylight saving)** and calculates DST internally.

This means:

- if the ESP32 sends already DST-adjusted local time, the display may be off by one hour in summer

## Recommended Time Strategy

### Option A: Best compatibility with the current Teensy code

The ESP32 sends **standard time without DST offset**.

Example:

- actual local summer time: `18:12`
- send to the Teensy: `17:12`

Then the existing DST logic in the Teensy remains correct.

### Option B: Later architecture cleanup

Store the RTC as true local time and remove or rewrite the DST logic in the Teensy code.

Recommendation:

- start with Option A

## Recommended Data Format

For UART, a plain text format is the best choice here:

```text
2026-03-28 18:12:34
```

with line ending:

```text
\r\n
```

This format matches the existing Teensy logic directly.

## Communication Model

### Simple first version

- the ESP32 sends exactly one time line after a successful NTP sync
- the Teensy reads it and sets the RTC

### Extended version

- the ESP32 sends once at startup
- optionally sends again periodically, for example every 6 or 12 hours
- the Teensy ignores invalid or incomplete lines

## Implementation in `NTP_2`

1. Obtain NTP time reliably
2. Convert to standard time without DST if required
3. Format the time as `YYYY-MM-DD HH:MM:SS`
4. initialize a second serial channel on the ESP32
5. send the time line to the Teensy

Suggested new function:

```cpp
void sendTimeToTeensy(const tm& timeinfo);
```

This function should:

- create a string in the existing Teensy format
- optionally append `\r\n`
- write it to the selected ESP32 UART

## Implementation in `myMatrixClock2`

1. Keep the existing parsing logic
2. Generalize `setRTCFromSerial()`
3. Make the same RTC-setting logic usable for both `Serial` and `Serial1`

Suggested new structure:

```cpp
bool setRtcFromStream(Stream& stream);
```

or alternatively:

```cpp
bool setRtcFromLine(const char* input);
```

Then:

- `Serial` remains for USB input from the PC
- `Serial1` is used for NTP time from the ESP32

## Recommended Refactor in `myMatrixClock2`

### Step 1

Keep `parseRtcInput()` unchanged

### Step 2

Create a shared function for setting the RTC from parsed data

For example:

```cpp
bool setRtcFromParsedTime(const tmElements_t& normalized, time_t parsedTime);
```

### Step 3

Refactor `setRTCFromSerial()` into a generic stream version:

```cpp
bool setRTCFromStream(Stream& stream);
```

### Step 4

Initialize `Serial1.begin(...)` in `setup()`

### Step 5

Check both sources in `loop()`:

```cpp
if (Serial.available() > 0) {
    setRTCFromStream(Serial);
}

if (Serial1.available() > 0) {
    setRTCFromStream(Serial1);
}
```

## Debug Output

### ESP32

- NTP source
- transmitted time line
- optional `UART_TX` label

### Teensy

- received time line on `Serial`
- RTC successfully set or error

## Error Cases the Teensy Should Handle

- empty line
- incomplete line
- wrong format
- invalid date
- invalid time

On error:

- discard the input
- leave the RTC unchanged
- report the error over USB serial

## Test Plan

### Test 1: Local parser test on the Teensy

- send manually over USB `Serial`:

```text
2026-03-28 18:12:34
```

- verify that the RTC is set correctly

### Test 2: Basic UART test

- ESP32 sends a fixed time line
- Teensy prints the received line over USB serial

### Test 3: RTC set test

- Teensy accepts the UART time
- RTC is updated
- the display shows the correct time

### Test 4: DST test

- test summer and winter cases explicitly
- make sure there is no double DST offset

## Recommended Implementation Order

1. `myMatrixClock2`: decouple RTC-setting logic from `Serial`
2. `myMatrixClock2`: enable `Serial1`
3. `NTP_2`: generate a time line in the Teensy format
4. `NTP_2`: send the time over a second UART
5. test the wiring
6. only then add periodic resync logic

## Clear Recommendation

For the first working version:

- ESP32 as NTP source
- Teensy remains the RTC and display system
- transfer over hardware UART
- USB serial remains free for debugging
- data format exactly matches the existing manual input format

## Next Sensible Step

The next logical step would be to refactor `myMatrixClock2/src/main.cpp` so the existing RTC-setting logic can work with both `Serial` and `Serial1`.

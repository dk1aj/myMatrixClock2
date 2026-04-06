# Plan for Transferring NTP Time from `NTP_2` to `myMatrixClock2` via SPI

## Goal

The time received by the ESP32 project `NTP_2` via Wi-Fi/NTP should be transferred to the Teensy project `myMatrixClock2`, so the RTC can be set there and the matrix clock can continue running independently afterward.

## Selected Interface

The transfer will use SPI, with the ESP32 as SPI master and the Teensy 3.1 as SPI slave.

This fits the required communication pattern well:

- the ESP32 decides when fresh NTP time is available
- the ESP32 can actively push the data
- the Teensy only needs to receive, validate, and write the RTC
- USB serial has to remain unchanged and available for debugging and manual input

## Explicit Pin Assignment

Use these SPI pins explicitly on the Teensy side:

- `Pin 15` as `CS`
- `Pin 11` as `MOSI`
- `Pin 13` as `CLK`
- `Pin 12` as `MISO`

## Wiring

### ESP32 to Teensy

- ESP32 `GPIO5` -> Teensy `Pin 15`
- ESP32 `GPIO23` -> Teensy `Pin 11`
- ESP32 `GPIO18` -> Teensy `Pin 13`
- ESP32 `GPIO19` -> Teensy `Pin 12`
- common `GND`

Signal names for `esp32dev`:

- ESP32 `CS` -> Teensy `Pin 15`
- ESP32 `MOSI` -> Teensy `Pin 11`
- ESP32 `SCK` -> Teensy `Pin 13`
- ESP32 `MISO` -> Teensy `Pin 12`
- common `GND`

## Communication Direction

### First implementation

- ESP32 sends time to the Teensy
- Teensy receives the payload and sets the RTC
- `MISO` is wired and reserved, but can remain unused in the first version if no response frame is needed

### Later extension

- Teensy can return a short acknowledge or status byte over `MISO`
- this can confirm:
  - packet accepted
  - format error
  - RTC write failure

## Current Time-Base Model in `myMatrixClock2`

The current Teensy code now behaves like this:

- the RTC stores local civil time
- in winter this means CET
- in summer this means CEST
- the main display shows the RTC hour directly
- UTC/Z is calculated from local time by subtracting `UTC+1` plus the DST offset

Important consequence:

- the ESP32 should send the real local time that should appear on the clock display
- do not force winter time only
- do not subtract DST on the ESP32 before transmission

Example:

- if actual local summer time is `18:12:34`
- the ESP32 should send `18:12:34`
- the Teensy stores `18:12:34` in the RTC
- the Teensy itself derives the UTC/Z line for display

## Recommended Data Format

Use a plain ASCII payload matching the existing Teensy parser:

```text
YYYY-MM-DD HH:MM:SS
```

Example:

```text
2026-03-28 18:12:34
```

Recommended frame strategy for SPI:

1. ESP32 asserts `CS`
2. ESP32 transmits a fixed-size ASCII buffer
3. Teensy collects the bytes while `CS` is active
4. ESP32 deasserts `CS`
5. Teensy validates the received line and sets the RTC

## Recommended SPI Payload Shape

For a simple first version, use a fixed 32-byte transmit buffer:

- bytes `0..18`: ASCII time string `YYYY-MM-DD HH:MM:SS`
- byte `19`: null terminator `\0`
- remaining bytes: zero-filled padding

This is simple to debug and easy to parse.

Alternative later:

- add a start marker, payload length, and checksum
- only needed if basic fixed-length transfer proves unreliable

## Communication Model

### Simple first version

- ESP32 performs NTP sync
- ESP32 formats the current local time
- ESP32 sends one SPI frame to the Teensy
- Teensy parses the line and sets the RTC

### Extended version

- ESP32 sends once at startup
- ESP32 sends again periodically, for example every 6 or 12 hours
- Teensy ignores incomplete or invalid frames

## Current Project Status

### `NTP_2`

- runs on ESP32
- receives time via Wi-Fi/NTP
- already has access to structured time information that can be formatted for transmission

### `myMatrixClock2`

- runs on Teensy 3.1
- uses `DS1307RTC`
- already parses time in the format:

```text
YYYY-MM-DD HH:MM:SS
```

Important reusable building blocks already present:

- `parseRtcInput()`
- `RTC.write(normalized)`
- `setTime(parsedTime)`

## Implementation in `myMatrixClock2`

### Required refactor

The RTC-setting logic should be separated from the current USB-only input path.

Recommended internal structure:

```cpp
bool setRtcFromParsedTime(const tmElements_t& normalized, time_t parsedTime);
bool setRtcFromLine(const char* input);
```

Then:

- USB manual input can still use `setRtcFromLine()`
- SPI reception can also use `setRtcFromLine()`

### SPI handling on Teensy

1. Initialize SPI slave support using the selected pins
2. Detect an active `CS` on `Pin 15`
3. Read the incoming bytes clocked on `Pin 13`
4. Collect the ASCII payload received on `Pin 11`
5. Optionally prepare a reply byte stream on `Pin 12`
6. After `CS` goes inactive, validate the frame
7. Pass the extracted line into `setRtcFromLine()`

### USB behavior remains unchanged

- `Serial` stays available as USB debug and manual input
- SPI is only used for the automatic time handoff from the ESP32

## Implementation in `NTP_2`

1. Obtain NTP time reliably
2. Convert it to the local time that should actually be shown on the clock
3. Format the time as `YYYY-MM-DD HH:MM:SS`
4. Initialize SPI using the agreed pin mapping
5. Drive `CS`
6. Send the fixed-size ASCII frame

Suggested new function:

```cpp
void sendTimeToTeensy();
```

This function should:

- build the ASCII time line
- write it over SPI in one transaction
- deassert `CS` only after the full frame is sent

## Recommended Step-by-Step Refactor in `myMatrixClock2`

### Step 1

Keep `parseRtcInput()` unchanged.

### Step 2

Create a shared function:

```cpp
bool setRtcFromParsedTime(const tmElements_t& normalized, time_t parsedTime);
```

This function should:

- write the RTC
- update the system time with `setTime(parsedTime)`
- print a success or error message over USB serial

### Step 3

Create:

```cpp
bool setRtcFromLine(const char* input);
```

This function should:

- call `parseRtcInput()`
- call `setRtcFromParsedTime()`

### Step 4

Keep the current USB input path, but make it call `setRtcFromLine()`.

### Step 5

Add SPI slave receive logic for:

- `CS` on `Pin 15`
- `MOSI` on `Pin 11`
- `CLK` on `Pin 13`
- `MISO` on `Pin 12`

### Step 6

After a complete SPI frame is received, pass the decoded line to `setRtcFromLine()`.

### Step 7

Add optional SPI response codes on `MISO`.

Suggested later response values:

- `0x00` = idle
- `0x01` = accepted
- `0x02` = parse error
- `0x03` = RTC write failed

## Debug Output

### ESP32

- print when NTP sync succeeds
- print the exact line sent to the Teensy
- optionally print SPI transaction status

### Teensy

- print when an SPI frame is received
- print the received decoded line over USB serial
- print whether RTC update succeeded or failed

## Error Cases the Teensy Should Handle

- empty SPI frame
- truncated frame
- missing null terminator
- wrong ASCII format
- invalid calendar date
- invalid time
- RTC write failure

On error:

- discard the frame
- leave the RTC unchanged
- report the error over USB serial

## Test Plan

### Test 1: Local parser test on the Teensy

- send manually over USB:

```text
2026-03-28 18:12:34
```

- verify that the RTC is set correctly

### Test 2: Basic SPI electrical test

- verify `CS`, `MOSI`, `CLK`, and `MISO` wiring
- verify that the Teensy detects `CS` on `Pin 15`
- verify that bytes sent from the ESP32 arrive correctly

### Test 3: Fixed-frame receive test

- ESP32 sends a hard-coded time string via SPI
- Teensy prints the received line over USB serial

### Test 4: RTC set test

- Teensy accepts the SPI time frame
- RTC is updated
- the display shows the correct local time

### Test 5: UTC/Z verification

- verify that the UTC/Z line is derived correctly from the received local time

### Test 6: Summer and winter test

- test one known winter date and one known summer date explicitly
- verify there is no one-hour display error

## Recommended Next Action

Start on the Teensy side first:

1. extract `setRtcFromParsedTime()`
2. add `setRtcFromLine()`
3. keep USB input working through the new shared path
4. only then add SPI slave reception

This reduces risk and keeps the parser/debug path stable while the SPI layer is added.

## Concrete Execution Plan

### Phase 1: Stabilize the RTC-setting path on the Teensy

Status:

- completed

Objective:

- isolate the time parsing and RTC-writing logic so it can be reused by both USB and SPI input

Work items:

1. Create `setRtcFromParsedTime(const tmElements_t& normalized, time_t parsedTime)`.
2. Move `RTC.write(normalized)` and `setTime(parsedTime)` into that function.
3. Create `setRtcFromLine(const char* input)`.
4. Make `setRtcFromLine()` call `parseRtcInput()` and then `setRtcFromParsedTime()`.
5. Refactor the current USB input path so it only collects a line and passes it to `setRtcFromLine()`.

Deliverable:

- one shared RTC-setting path used by USB input

Done criterion:

- manual USB input still sets the RTC correctly

Implementation status:

- `setRtcFromParsedTime(...)` exists
- `setRtcFromLine(...)` exists
- USB line input now routes through the shared RTC-setting path

### Phase 2: Define the SPI frame and local buffers

Status:

- completed for frame definition, local buffer helpers, and SPI transport logic

Objective:

- make the wire protocol explicit before implementing SPI receive logic

Work items:

1. Define a fixed frame size of `32` bytes.
2. Reserve bytes `0..18` for `YYYY-MM-DD HH:MM:SS`.
3. Reserve byte `19` for `\0`.
4. Zero-fill the remaining bytes.
5. Define a Teensy-side receive buffer and a small frame-state structure.

Deliverable:

- a fixed SPI frame contract shared by both projects

Done criterion:

- the frame format is documented and used consistently in code comments and implementation

Implementation status:

- fixed frame size `32` bytes is defined
- ASCII payload length `19` is defined
- null terminator position is defined explicitly
- a Teensy-side `SpiTimeFrameState` buffer exists
- helper functions for reset and safe line extraction exist

### Phase 3: Add SPI transmit on `NTP_2`

Status:

- completed

Objective:

- send a valid time frame from the ESP32 after NTP sync

Work items:

1. Initialize SPI on `esp32dev`.
2. Use:
   - `GPIO5` as `CS`
   - `GPIO23` as `MOSI`
   - `GPIO18` as `CLK`
   - `GPIO19` as `MISO`
3. Add a function `sendTimeToTeensy()`.
4. Format the current local time as `YYYY-MM-DD HH:MM:SS`.
5. Fill the 32-byte frame.
6. Pull `CS` low, transmit the full frame, then release `CS`.
7. Print the sent frame to serial debug output.

Deliverable:

- ESP32 sends a deterministic SPI frame containing local time

Done criterion:

- serial debug output shows the exact transmitted frame

Implementation status:

- `NTP_2` now sends the fixed 32-byte SPI frame
- `esp32dev` pin mapping is in place:
  - `GPIO5` = `CS`
  - `GPIO23` = `MOSI`
  - `GPIO18` = `CLK`
  - `GPIO19` = `MISO`

### Phase 4: Add SPI receive on the Teensy

Status:

- completed for frame reception, USB debug output, and RTC write-back from SPI

Objective:

- receive the SPI frame and decode it into an ASCII line

Work items:

1. Initialize SPI slave mode on the Teensy.
2. Use:
   - `Pin 15` as `CS`
   - `Pin 11` as `MOSI`
   - `Pin 13` as `CLK`
   - `Pin 12` as `MISO`
3. Detect frame start with active `CS`.
4. Collect bytes while `CS` is active.
5. Stop collection when `CS` returns inactive.
6. Null-terminate the local receive buffer safely.
7. Print the received frame over USB serial for inspection.

Deliverable:

- Teensy receives and prints complete SPI frames

Done criterion:

- a hard-coded frame from the ESP32 appears correctly on USB serial

Implementation status:

- SPI slave pins are configured on the Teensy
- SPI0 is initialized for slave reception
- chip select on `Pin 15` is used to delimit frames
- received bytes are stored into the existing `SpiTimeFrameState`
- completed frames are decoded, printed over USB serial as `SPI RX: [...]`,
  and valid frames are passed into `setRtcFromLine()`

### Phase 5: Connect SPI receive to RTC update

Objective:

- use the received SPI frame to set the RTC

Work items:

1. Pass the decoded line from the SPI receive buffer into `setRtcFromLine()`.
2. Reject invalid, truncated, or empty frames.
3. Keep the RTC unchanged on parse or write errors.
4. Print success and failure reasons over USB serial.

Deliverable:

- valid SPI frames update the RTC

Done criterion:

- the display updates to the transmitted local time
- valid SPI frames set the RTC through the shared USB/SPI path

### Phase 6: Optional reply path on `MISO`

Objective:

- give the ESP32 a minimal acknowledgement path

Work items:

1. Define one-byte reply codes.
2. Return:
   - `0x01` for accepted
   - `0x02` for parse error
   - `0x03` for RTC write failed
3. Keep `0x00` as idle/default.

Deliverable:

- minimal slave-to-master status reporting

Done criterion:

- ESP32 can distinguish success from failure without relying only on USB debug output
- one-byte reply/status codes are latched on `MISO`

### Phase 7: Integration validation

Objective:

- verify the complete chain from NTP sync to displayed time

Work items:

1. Test manual USB time setting again to confirm no regression.
2. Test ESP32 fixed-frame SPI send.
3. Test real NTP-derived SPI send.
4. Verify displayed local time.
5. Verify derived UTC/Z line.
6. Verify one explicit winter date and one explicit summer date.

Deliverable:

- end-to-end verified time transfer

Done criterion:

- the clock shows correct local time and correct UTC/Z in both winter and summer cases

## Final Status

Implementation status:

- `0x00` idle/default is exposed on `MISO`
- `0x01` is returned for accepted RTC updates
- `0x02` is returned for parse/invalid frame errors
- `0x03` is returned for RTC write failures
- USB debug output also prints the currently latched SPI reply code
- end-to-end validation with real SPI traffic from `NTP_2` is completed
- displayed local time matches the transmitted local time
- derived `UTC/Z` remains correct in both summer and winter cases

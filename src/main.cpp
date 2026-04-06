/******************************************************************************
 * Project     : Display Matrix Clock
 * File        : main.cpp
 * Author      : DK1AJ
 * Date        : 26.03.2026
 * Version     : 1.0
 *
 * Description :
 * Control software for a display matrix clock.
 *
 * License     :
 * This code may be used, modified, and redistributed freely.
 * Provided without warranty or liability.
 *
 * Notes       :
 * - Please document all changes
 * - Keep the original author credit
 *
 * Change Log  :
 * - Refactored the clock control into smaller helper functions for RTC sync,
 *   serial input parsing, UTC calculation, brightness handling, and rendering.
 * - Corrected the displayed local time so the RTC time is not shifted twice
 *   during daylight saving time.
 * - Added UTC/Z display on a separate red text layer with a yellow trailing
 *   "Z" marker.
 * - Moved the UTC/Z line upward and fine-tuned the "Z" position for better
 *   spacing on the 32x32 matrix.
 * - Centered the date display horizontally.
 * - Added a dedicated colored status layer:
 *   Sommer = warm orange, Winter = cool blue.
 * - Switched serial output and input handling to USB serial.
 *
 * SPI Phases   :
 * - Phase 1 completed:
 *   RTC write logic extracted into shared helpers
 *   `setRtcFromParsedTime(...)` and `setRtcFromLine(...)`,
 *   USB input now uses the shared path.
 * - Phase 2 completed:
 *   fixed SPI time frame defined with 32-byte buffer,
 *   19-character ASCII timestamp, explicit null terminator,
 *   local frame state, reset helper, and safe line extraction helper.
 * - Phase 3 completed:
 *   SPI transmit implemented in `NTP_2` on `esp32dev`.
 * - Phase 4 completed:
 *   Teensy-side SPI slave receive plus cyclical USB SPI status/debug output
 *   implemented without RTC write-back from SPI yet.
 * - Phase 5 completed:
 *   valid decoded SPI frames now pass into `setRtcFromLine(...)`, so SPI
 *   updates the RTC through the same shared path as USB input.
 * - Phase 6 completed:
 *   one-byte SPI reply/status codes are now latched on `MISO`:
 *   `0x00` idle, `0x01` accepted, `0x02` parse error, `0x03` RTC write failed.
 * - Phase 7 completed:
 *   end-to-end validation confirmed local time, UTC/Z, summer time, and
 *   winter time behavior.
 ******************************************************************************/

#include <Arduino.h>
#include <string.h>
#include <Wire.h>
#include <Time.h>
#include <DS1307RTC.h>
#include <SmartMatrix3.h>
#include <usb_serial.h>

#define COLOR_DEPTH 24

namespace
{
constexpr uint8_t kMatrixWidth = 32;
constexpr uint8_t kMatrixHeight = 32;
constexpr uint8_t kRefreshDepth = 24;
constexpr uint8_t kDmaBufferRows = 4;
constexpr uint8_t kPanelType = SMARTMATRIX_HUB75_32ROW_MOD16SCAN;
constexpr uint8_t kMatrixOptions = SMARTMATRIX_OPTIONS_NONE;
constexpr uint8_t kBackgroundLayerOptions = SM_BACKGROUND_OPTIONS_NONE;
constexpr uint8_t kIndexedLayerOptions = SM_INDEXED_OPTIONS_NONE;

constexpr int kNightBrightness = 0;
constexpr int kDayBrightness = 10;

constexpr unsigned long kDisplayUpdateIntervalMs = 1000;
constexpr unsigned long kStartupPromptDelayMs = 2000;
constexpr unsigned long kStartupSplashDurationMs = 5000;
constexpr uint32_t kSerialBaud = 9600;
constexpr size_t kSpiTimeFrameSize = 32;
constexpr size_t kSpiTimeTextLength = 19;
constexpr size_t kSpiTimeTerminatorIndex = kSpiTimeTextLength;
constexpr char kSpiStatusPollMarker[] = "STATUS?";
constexpr size_t kSpiStatusPollMarkerLength = sizeof(kSpiStatusPollMarker) - 1;
constexpr unsigned long kSpiBitTimeoutUs = 50000;
constexpr uint8_t kSpiCsPin = 15;
constexpr uint8_t kSpiSoutPin = 12;
constexpr uint8_t kSpiClkPin = 13;
constexpr uint8_t kSpiSinPin = 11;

constexpr uint8_t kDstStartMonth = 3;
constexpr uint8_t kDstEndMonth = 10;
constexpr uint8_t kDstStartHour = 2;
constexpr uint8_t kDstEndHour = 3;
constexpr uint8_t kNightModeStartHour = 20;
constexpr uint8_t kNightModeEndHour = 8;
constexpr int kStandardUtcOffsetHours = 1;

enum class UtcFontMode : uint8_t
{
    Compact3x5,
    Large5x7
};

constexpr UtcFontMode kUtcFontMode = UtcFontMode::Large5x7;
constexpr char kStartupLabel[] = "DK1AJ";
constexpr char kTimeInputFormat[] = "YYYY-MM-DD HH:MM:SS + CR/LF";
const rgb24 kUtcSuffixColors[] =
{
    {0xff, 0xff, 0x00},
    {0x00, 0xff, 0xff},
    {0xff, 0x80, 0x20},
    {0x80, 0xff, 0x40},
    {0xff, 0x40, 0xa0}
};
constexpr size_t kUtcSuffixColorCount = sizeof(kUtcSuffixColors) / sizeof(kUtcSuffixColors[0]);
} // namespace

usb_serial_class& USB_serial = Serial;

SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
SMARTMATRIX_ALLOCATE_BACKGROUND_LAYER(backgroundLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kBackgroundLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(indexedLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(utcLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(utcSuffixLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(statusLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

struct SpiTimeFrameState
{
    char frame[kSpiTimeFrameSize];
    size_t receivedBytes;
    bool frameReady;
};

enum class RtcUpdateResult : uint8_t
{
    Accepted,
    ParseError,
    RtcWriteFailed
};

enum class SpiReplyCode : uint8_t
{
    Idle = 0x00,
    Accepted = 0x01,
    ParseError = 0x02,
    RtcWriteFailed = 0x03
};

bool dst = true;
unsigned long lastDisplayUpdate = 0;
unsigned long setupStartMillis = 0;
bool startupMessageShown = false;
tmElements_t tm;
tmElements_t utcTime;
SpiTimeFrameState spiTimeFrame = {};
bool spiChipSelectActive = false;
uint8_t spiReplyByte = static_cast<uint8_t>(SpiReplyCode::Idle);
size_t utcSuffixColorIndex = 0;

RtcUpdateResult setRtcFromLine(const char* input);

/**
 * Advances the display color used for the trailing UTC `Z` marker.
 */
void advanceUtcSuffixColor(void)
{
    utcSuffixColorIndex = (utcSuffixColorIndex + 1) % kUtcSuffixColorCount;
}

/**
 * Maps a raw SPI reply byte to a human-readable label for USB diagnostics.
 */
const char* spiReplyLabel(uint8_t replyCode)
{
    switch (static_cast<SpiReplyCode>(replyCode))
    {
        case SpiReplyCode::Accepted:
            return "accepted";
        case SpiReplyCode::ParseError:
            return "parse-error";
        case SpiReplyCode::RtcWriteFailed:
            return "rtc-write-failed";
        case SpiReplyCode::Idle:
            return "idle";
        default:
            return "custom";
    }
}

/**
 * Stores the reply byte that will be shifted out on MISO during the next SPI
 * transfer and preloads its first output bit onto the output pin.
 */
void loadSpiReplyByte(uint8_t replyCode)
{
    spiReplyByte = replyCode;
    digitalWrite(kSpiSoutPin, (spiReplyByte & 0x80U) != 0 ? HIGH : LOW);
}

/**
 * Prints the currently selected SPI reply code in a compact debug format.
 */
void printSpiReplyDebug(uint8_t replyCode)
{
    char replyHex[5] = {};
    snprintf(replyHex, sizeof(replyHex), "%02X", replyCode);

    USB_serial.print("SPI reply: 0x");
    USB_serial.print(replyHex);
    USB_serial.print(' ');
    USB_serial.println(spiReplyLabel(replyCode));
}

/**
 * Dumps all bytes captured for the current SPI frame as hexadecimal values.
 * This is only used for debugging invalid or incomplete transfers.
 */
void printSpiFrameDebug(const SpiTimeFrameState& state)
{
    USB_serial.print("SPI frame bytes:");

    for (size_t i = 0; i < state.receivedBytes; ++i)
    {
        char byteHex[4] = {};
        snprintf(byteHex, sizeof(byteHex), " %02X", static_cast<uint8_t>(state.frame[i]));
        USB_serial.print(byteHex);
    }

    USB_serial.println();
}

/**
 * Updates the latched SPI reply code and mirrors the change to the USB debug
 * output so master and slave behavior can be correlated.
 */
void setSpiReplyCode(SpiReplyCode replyCode)
{
    loadSpiReplyByte(static_cast<uint8_t>(replyCode));
    printSpiReplyDebug(spiReplyByte);
}

/**
 * Converts an RTC update result into the corresponding one-byte SPI status
 * code used by the ESP32 master.
 */
SpiReplyCode spiReplyCodeForRtcResult(RtcUpdateResult result)
{
    switch (result)
    {
        case RtcUpdateResult::Accepted:
            return SpiReplyCode::Accepted;
        case RtcUpdateResult::ParseError:
            return SpiReplyCode::ParseError;
        case RtcUpdateResult::RtcWriteFailed:
        default:
            return SpiReplyCode::RtcWriteFailed;
    }
}

/**
 * Resets the local SPI frame buffer and bookkeeping so a fresh transfer can be
 * collected without stale payload data.
 */
void resetSpiTimeFrameState(SpiTimeFrameState& state)
{
    memset(state.frame, 0, sizeof(state.frame));
    state.receivedBytes = 0;
    state.frameReady = false;
}

/**
 * Searches the received SPI frame for a valid timestamp in
 * `YYYY-MM-DD HH:MM:SS` format followed by a null terminator.
 *
 * The implementation tolerates a shifted start offset inside the fixed-size
 * 32-byte frame so slightly misaligned transfers can still be recovered.
 */
bool extractSpiTimeLine(const SpiTimeFrameState& state, char* line, size_t lineSize)
{
    if (lineSize < kSpiTimeTextLength + 1)
    {
        return false;
    }

    if (state.receivedBytes == 0 || state.receivedBytes > kSpiTimeFrameSize)
    {
        return false;
    }

    auto matchesTimestampPattern = [](const char* candidate) -> bool
    {
        return
            candidate[4] == '-' &&
            candidate[7] == '-' &&
            candidate[10] == ' ' &&
            candidate[13] == ':' &&
            candidate[16] == ':' &&
            candidate[0] >= '0' && candidate[0] <= '9' &&
            candidate[1] >= '0' && candidate[1] <= '9' &&
            candidate[2] >= '0' && candidate[2] <= '9' &&
            candidate[3] >= '0' && candidate[3] <= '9' &&
            candidate[5] >= '0' && candidate[5] <= '9' &&
            candidate[6] >= '0' && candidate[6] <= '9' &&
            candidate[8] >= '0' && candidate[8] <= '9' &&
            candidate[9] >= '0' && candidate[9] <= '9' &&
            candidate[11] >= '0' && candidate[11] <= '9' &&
            candidate[12] >= '0' && candidate[12] <= '9' &&
            candidate[14] >= '0' && candidate[14] <= '9' &&
            candidate[15] >= '0' && candidate[15] <= '9' &&
            candidate[17] >= '0' && candidate[17] <= '9' &&
            candidate[18] >= '0' && candidate[18] <= '9';
    };

    const size_t maxStart = (state.receivedBytes >= (kSpiTimeTextLength + 1))
        ? (state.receivedBytes - (kSpiTimeTextLength + 1))
        : 0;

    for (size_t start = 0; start <= maxStart; ++start)
    {
        if (!matchesTimestampPattern(&state.frame[start]))
        {
            continue;
        }

        if (state.frame[start + kSpiTimeTerminatorIndex] != '\0')
        {
            continue;
        }

        memcpy(line, &state.frame[start], kSpiTimeTextLength + 1);
        return true;
    }

    return false;
}

/**
 * Detects whether the current frame is the ESP32 status poll payload instead
 * of a time-setting frame.
 */
bool isSpiStatusPollFrame(const SpiTimeFrameState& state)
{
    if (state.receivedBytes == 0 || state.receivedBytes > kSpiTimeFrameSize)
    {
        return false;
    }

    if (kSpiStatusPollMarkerLength + 1 >= kSpiTimeFrameSize)
    {
        return false;
    }

    if (state.frame[kSpiStatusPollMarkerLength] != '\0')
    {
        return false;
    }

    return memcmp(state.frame, kSpiStatusPollMarker, kSpiStatusPollMarkerLength) == 0;
}

/**
 * Configures the Teensy GPIO pins used for the software SPI slave role.
 */
void configureSpiSlavePins(void)
{
    pinMode(kSpiCsPin, INPUT_PULLUP);
    pinMode(kSpiClkPin, INPUT);
    pinMode(kSpiSinPin, INPUT);
    pinMode(kSpiSoutPin, OUTPUT);
    digitalWrite(kSpiSoutPin, LOW);
}

/**
 * Initializes the SPI slave state after pin configuration and reports the
 * chosen wiring over USB serial.
 */
void initializeSpiSlave(void)
{
    loadSpiReplyByte(static_cast<uint8_t>(SpiReplyCode::Idle));

    USB_serial.print("SPI slave ready: CS=");
    USB_serial.print(kSpiCsPin);
    USB_serial.print(" SIN=");
    USB_serial.print(kSpiSinPin);
    USB_serial.print(" SOUT=");
    USB_serial.print(kSpiSoutPin);
    USB_serial.print(" CLK=");
    USB_serial.println(kSpiClkPin);
}

/**
 * Waits until the SPI clock reaches the requested level while chip select
 * remains active. Returns `false` on timeout or transfer end.
 */
bool waitForSpiClockState(int expectedState)
{
    const unsigned long start = micros();

    while (digitalRead(kSpiCsPin) == LOW)
    {
        if (digitalRead(kSpiClkPin) == expectedState)
        {
            return true;
        }

        if (micros() - start >= kSpiBitTimeoutUs)
        {
            return false;
        }
    }

    return false;
}

/**
 * Receives one SPI byte bit by bit while presenting the currently latched
 * reply byte on MISO. Returns `false` if the transfer aborts mid-byte.
 */
inline bool receiveSpiByte(uint8_t& receivedByte)
{
    receivedByte = 0;
    uint8_t replyByte = spiReplyByte;

    for (uint8_t bitIndex = 0; bitIndex < 8; ++bitIndex)
    {
        digitalWrite(kSpiSoutPin, (replyByte & 0x80U) != 0 ? HIGH : LOW);

        if (!waitForSpiClockState(HIGH))
        {
            return false;
        }

        receivedByte = static_cast<uint8_t>(
            (receivedByte << 1) | (digitalRead(kSpiSinPin) == HIGH ? 1U : 0U));
        replyByte <<= 1;

        if (!waitForSpiClockState(LOW))
        {
            return false;
        }
    }

    return true;
}

/**
 * Collects a full fixed-length SPI frame while chip select is active.
 *
 * The routine is intentionally conservative: it stops immediately on timeout
 * or early chip-select release and marks the partial frame for later analysis.
 */
void pollSpiTimeFrame(void)
{
    if (digitalRead(kSpiCsPin) != LOW)
    {
        return;
    }

    spiChipSelectActive = true;
    resetSpiTimeFrameState(spiTimeFrame);

    while (digitalRead(kSpiCsPin) == LOW && spiTimeFrame.receivedBytes < kSpiTimeFrameSize)
    {
        uint8_t receivedByte = 0;

        if (!receiveSpiByte(receivedByte))
        {
            goto transfer_done;
        }

        spiTimeFrame.frame[spiTimeFrame.receivedBytes++] = static_cast<char>(receivedByte);
    }

transfer_done:
    spiChipSelectActive = false;
    digitalWrite(kSpiSoutPin, (spiReplyByte & 0x80U) != 0 ? HIGH : LOW);

    if (spiTimeFrame.receivedBytes > 0)
    {
        spiTimeFrame.frameReady = true;
    }
}

/**
 * Evaluates the most recently captured SPI frame, routes valid timestamps into
 * the shared RTC update path, and updates the SPI reply code accordingly.
 */
void processSpiTimeFrame(void)
{
    if (!spiTimeFrame.frameReady)
    {
        return;
    }

    if (isSpiStatusPollFrame(spiTimeFrame))
    {
        USB_serial.println("SPI status poll");
        resetSpiTimeFrameState(spiTimeFrame);
        return;
    }

    char timeLine[kSpiTimeTextLength + 1] = {};

    if (extractSpiTimeLine(spiTimeFrame, timeLine, sizeof(timeLine)))
    {
        USB_serial.print("SPI RX: [");
        USB_serial.print(timeLine);
        USB_serial.println(']');

        const RtcUpdateResult updateResult = setRtcFromLine(timeLine);
        if (updateResult == RtcUpdateResult::Accepted)
        {
            advanceUtcSuffixColor();
        }
        setSpiReplyCode(spiReplyCodeForRtcResult(updateResult));

        if (updateResult != RtcUpdateResult::Accepted)
        {
            USB_serial.println("SPI RTC update failed");
        }
    }
    else
    {
        USB_serial.print("SPI RX invalid, bytes=");
        USB_serial.println(spiTimeFrame.receivedBytes);
        printSpiFrameDebug(spiTimeFrame);
        setSpiReplyCode(SpiReplyCode::ParseError);
    }

    resetSpiTimeFrameState(spiTimeFrame);
}

/**
 * Returns whether the given calendar year is a leap year in the Gregorian
 * calendar.
 */
bool isLeapYear(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

/**
 * Returns the last valid day number for a given month and year.
 *
 * Invalid months are clamped to a harmless fallback value so callers cannot
 * index past the month table accidentally.
 */
uint8_t lastDayOfMonth(int year, int month)
{
    static const uint8_t kDaysInMonth[] =
    {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
    {
        return 31;
    }

    if (month == 2 && isLeapYear(year))
    {
        return 29;
    }

    return kDaysInMonth[month - 1];
}

/**
 * Computes the day number of the last Sunday within the given month.
 */
uint8_t lastSundayOfMonth(int year, int month)
{
    const uint8_t lastDay = lastDayOfMonth(year, month);

    for (int day = lastDay; day >= lastDay - 6; --day)
    {
        tmElements_t date = {};
        date.Year = CalendarYrToTm(year);
        date.Month = month;
        date.Day = day;
        date.Hour = 12;

        const time_t t = makeTime(date);
        breakTime(t, date);

        if (date.Wday == 1)
        {
            return day;
        }
    }

    return lastDay;
}

/**
 * Determines whether local civil time should currently be interpreted as DST
 * according to the EU rule set used by this clock.
 */
bool isDstActive(const tmElements_t& rtcTime)
{
    if (rtcTime.Month < kDstStartMonth || rtcTime.Month > kDstEndMonth)
    {
        return false;
    }

    if (rtcTime.Month > kDstStartMonth && rtcTime.Month < kDstEndMonth)
    {
        return true;
    }

    const uint8_t changeDay = lastSundayOfMonth(tmYearToCalendar(rtcTime.Year), rtcTime.Month);

    if (rtcTime.Month == kDstStartMonth)
    {
        if (rtcTime.Day < changeDay)
        {
            return false;
        }

        if (rtcTime.Day > changeDay)
        {
            return true;
        }

        return rtcTime.Hour >= kDstStartHour;
    }

    if (rtcTime.Day < changeDay)
    {
        return true;
    }

    if (rtcTime.Day > changeDay)
    {
        return false;
    }

    return rtcTime.Hour < kDstEndHour;
}

/**
 * Derives UTC from the RTC's stored local civil time plus the computed DST
 * state and writes the result into the global UTC display buffer.
 */
void updateUtcTime(const tmElements_t& rtcTime, bool dstActive)
{
    const int utcOffsetHours = kStandardUtcOffsetHours + (dstActive ? 1 : 0);
    const time_t utcTimestamp = makeTime(rtcTime) - (utcOffsetHours * SECS_PER_HOUR);
    breakTime(utcTimestamp, utcTime);
}

/**
 * Returns the textual DST state shown on the matrix and on USB debug output.
 */
const char* dstLabel(bool dstActive)
{
    return dstActive ? "CEST" : "CET";
}

/**
 * Prints a decimal value with a leading zero when needed.
 */
void printTwoDigits(int value)
{
    if (value < 10)
    {
        USB_serial.print('0');
    }

    USB_serial.print(value);
}

/**
 * Emits the current local RTC time, date, and DST state over USB serial in a
 * format intended for manual monitoring.
 */
void printClockDisplay(const tmElements_t& rtcTime, bool dstActive)
{
    USB_serial.print(rtcTime.Hour);
    USB_serial.print(':');
    printTwoDigits(rtcTime.Minute);
    USB_serial.print(':');
    printTwoDigits(rtcTime.Second);
    USB_serial.print(' ');
    USB_serial.print(rtcTime.Day);
    USB_serial.print('.');
    USB_serial.print(rtcTime.Month);
    USB_serial.print('.');
    USB_serial.print(tmYearToCalendar(rtcTime.Year));
    USB_serial.print(' ');
    USB_serial.print(dstLabel(dstActive));
    USB_serial.print(" wday=");
    USB_serial.println(rtcTime.Wday);
}

/**
 * Emits the derived UTC time and date over USB serial.
 */
void printUtcClockDisplay(const tmElements_t& timeUtc)
{
    USB_serial.print("UTC ");
    USB_serial.print(timeUtc.Hour);
    USB_serial.print(':');
    printTwoDigits(timeUtc.Minute);
    USB_serial.print(':');
    printTwoDigits(timeUtc.Second);
    USB_serial.print(' ');
    USB_serial.print(timeUtc.Day);
    USB_serial.print('.');
    USB_serial.print(timeUtc.Month);
    USB_serial.print('.');
    USB_serial.println(tmYearToCalendar(timeUtc.Year));
}

/**
 * Prints a normalized `RTC set to:` line after a successful RTC update.
 */
void printNormalizedTimestamp(int year, int month, int day, int hour, int minute, int second)
{
    USB_serial.print("RTC set to: ");
    USB_serial.print(year);
    USB_serial.print('-');
    printTwoDigits(month);
    USB_serial.print('-');
    printTwoDigits(day);
    USB_serial.print(' ');
    printTwoDigits(hour);
    USB_serial.print(':');
    printTwoDigits(minute);
    USB_serial.print(':');
    printTwoDigits(second);
    USB_serial.println();
}

/**
 * Performs cheap range checks before more expensive calendar normalization is
 * attempted.
 */
bool hasBasicDateTimeRange(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 2000 && year <= 2099 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

/**
 * Parses an ASCII timestamp, validates it as a real calendar value, and
 * returns both normalized `tmElements_t` data and the matching `time_t`.
 */
bool parseRtcInput(const char* input, tmElements_t& normalized, time_t& parsedTime)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(input, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        USB_serial.print("Received: [");
        USB_serial.print(input);
        USB_serial.println(']');
        USB_serial.print("Invalid format. Please enter: ");
        USB_serial.println(kTimeInputFormat);
        return false;
    }

    if (!hasBasicDateTimeRange(year, month, day, hour, minute, second))
    {
        USB_serial.println("Invalid date/time values");
        return false;
    }

    tmElements_t candidate = {};
    candidate.Year = CalendarYrToTm(year);
    candidate.Month = month;
    candidate.Day = day;
    candidate.Hour = hour;
    candidate.Minute = minute;
    candidate.Second = second;

    parsedTime = makeTime(candidate);
    breakTime(parsedTime, normalized);

    if (tmYearToCalendar(normalized.Year) != year ||
        normalized.Month != month ||
        normalized.Day != day ||
        normalized.Hour != hour ||
        normalized.Minute != minute ||
        normalized.Second != second)
    {
        USB_serial.println("Invalid calendar date");
        return false;
    }

    return true;
}

/**
 * Writes a validated timestamp into the DS1307 RTC and mirrors it into the
 * Teensy system time base.
 */
RtcUpdateResult setRtcFromParsedTime(tmElements_t normalized, time_t parsedTime)
{
    if (!RTC.write(normalized))
    {
        USB_serial.println("Error: RTC write failed");
        return RtcUpdateResult::RtcWriteFailed;
    }

    setTime(parsedTime);
    printNormalizedTimestamp(
        tmYearToCalendar(normalized.Year),
        normalized.Month,
        normalized.Day,
        normalized.Hour,
        normalized.Minute,
        normalized.Second);
    return RtcUpdateResult::Accepted;
}

/**
 * Shared high-level entry point for ASCII timestamps coming from USB or SPI.
 */
RtcUpdateResult setRtcFromLine(const char* input)
{
    tmElements_t normalized = {};
    time_t parsedTime = 0;

    if (!parseRtcInput(input, normalized, parsedTime))
    {
        return RtcUpdateResult::ParseError;
    }

    return setRtcFromParsedTime(normalized, parsedTime);
}

/**
 * Discards the remainder of the current USB input line after an overflow.
 */
void discardSerialLine(void)
{
    while (USB_serial.available() > 0)
    {
        if (USB_serial.read() == '\n')
        {
            break;
        }
    }
}

/**
 * Reads one complete timestamp line from USB serial and forwards it to the
 * shared RTC update path.
 */
bool setRTCFromSerial(void)
{
    static char input[32];
    static uint8_t idx = 0;
    static bool ignoreNextLineFeed = false;

    while (USB_serial.available() > 0)
    {
        const char c = USB_serial.read();

        if (ignoreNextLineFeed && c == '\n')
        {
            ignoreNextLineFeed = false;
            continue;
        }

        if (ignoreNextLineFeed)
        {
            ignoreNextLineFeed = false;
        }

        if (c == '\r' || c == '\n')
        {
            ignoreNextLineFeed = (c == '\r');

            input[idx] = '\0';
            idx = 0;

            if (input[0] == '\0')
            {
                continue;
            }

            return setRtcFromLine(input) == RtcUpdateResult::Accepted;
        }

        if (c < 32 || c > 126)
        {
            continue;
        }

        if (idx < sizeof(input) - 1)
        {
            input[idx++] = c;
            continue;
        }

        idx = 0;
        discardSerialLine();
        USB_serial.println("Input too long");
        return false;
    }

    return false;
}

/**
 * Configures the I2C pins used by the RTC hardware.
 */
void configureRtcPins(void)
{
    pinMode(18, INPUT);
    pinMode(19, INPUT);
    CORE_PIN16_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
    CORE_PIN17_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
}

/**
 * Shows the startup splash screen for the configured boot duration and resets
 * display-related state before normal operation begins.
 */
void showStartupSplash(void)
{
    matrix.setBrightness(kDayBrightness);
    resetSpiTimeFrameState(spiTimeFrame);
    backgroundLayer.fillScreen({0x00, 0x00, 0x00});
    backgroundLayer.setFont(gohufont11b);
    backgroundLayer.drawString(0, kMatrixHeight / 2 - 6, {0xff, 0xff, 0xff}, kStartupLabel);
    backgroundLayer.swapBuffers(true);

    const unsigned long splashStart = millis();
    while (millis() - splashStart < kStartupSplashDurationMs)
    {
    }
}

/**
 * Synchronizes the Teensy time library with the RTC chip on startup.
 */
void syncTimeFromRtc(void)
{
    setSyncProvider(RTC.get);

    if (timeStatus() == timeSet)
    {
        USB_serial.println("RTC has set the system time");
    }
    else
    {
        USB_serial.println("Unable to sync with the RTC");
    }
}

/**
 * Prints a one-time usage hint on USB serial after startup.
 */
void maybeShowStartupPrompt(unsigned long now)
{
    if (startupMessageShown || (now - setupStartMillis < kStartupPromptDelayMs))
    {
        return;
    }

    USB_serial.print("Set time with: ");
    USB_serial.println(kTimeInputFormat);
    USB_serial.println("Example: 2026-03-25 14:30:00");
    startupMessageShown = true;
}

/**
 * Returns the hour that should be rendered on the display.
 *
 * The parameter list still carries the DST state so this helper can absorb
 * future display-policy changes without touching call sites.
 */
uint8_t displayHourFrom(const tmElements_t& rtcTime, bool dstActive)
{
    (void)dstActive;
    return rtcTime.Hour;
}

/**
 * Applies the configured day/night brightness policy based on the display hour.
 */
void applyBrightness(uint8_t displayHour)
{
    const bool nightMode = displayHour >= kNightModeStartHour || displayHour < kNightModeEndHour;
    matrix.setBrightness(nightMode ? kNightBrightness : kDayBrightness);
}

/**
 * Renders the complete 32x32 matrix view including local time, date, UTC/Z,
 * and summer/winter status.
 */
void drawClockScreen(const tmElements_t& rtcTime, bool dstActive)
{
    const uint8_t displayHour = displayHourFrom(rtcTime, dstActive);
    const bool showBlinkPixel = ((rtcTime.Second & 0x01U) == 0);
    constexpr int16_t kFont3x5Width = 3;
    constexpr int16_t kFont5x7Width = 5;
    constexpr int16_t kUtcTextLength = 5;
    constexpr int16_t kUtcXOffset = -2;
    char timeBuffer[8];
    char utcBuffer[8];
    char dateBuffer[6];

    applyBrightness(displayHour);
    backgroundLayer.fillScreen({0x00, 0x00, 0x00});

    snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d", displayHour, rtcTime.Minute);
    snprintf(utcBuffer, sizeof(utcBuffer), "%02d:%02d", utcTime.Hour, utcTime.Minute);
    const int dateLength = snprintf(dateBuffer, sizeof(dateBuffer), "%02d.%02d", rtcTime.Day, rtcTime.Month);

    backgroundLayer.setFont(gohufont11b);
    backgroundLayer.drawString(displayHour < 10 ? 3 : 1, 0, {0xff, 0xff, 0xff}, timeBuffer);

    backgroundLayer.setFont(font3x5);
    backgroundLayer.drawString(((kMatrixWidth - (dateLength * kFont3x5Width)) / 2) - 1, 19, {0xff, 0xff, 0xff}, dateBuffer);

    if (kUtcFontMode == UtcFontMode::Large5x7)
    {
        backgroundLayer.setFont(font5x7);
        backgroundLayer.drawString(((kMatrixWidth - (kUtcTextLength * kFont5x7Width)) / 2) + kUtcXOffset, 11, {0xff, 0x00, 0x00}, utcBuffer);
        backgroundLayer.setFont(font3x5);
        backgroundLayer.drawString(28, 12, kUtcSuffixColors[utcSuffixColorIndex], "Z");
    }
    else
    {
        backgroundLayer.setFont(font3x5);
        backgroundLayer.drawString(((kMatrixWidth - (kUtcTextLength * kFont3x5Width)) / 2) + kUtcXOffset, 11, {0xff, 0x00, 0x00}, utcBuffer);
        backgroundLayer.drawString(23, 11, kUtcSuffixColors[utcSuffixColorIndex], "Z");
    }

    const char* statusText = dstLabel(dstActive);
    const int16_t statusX = (kMatrixWidth - (static_cast<int16_t>(strlen(statusText)) * kFont3x5Width)) / 2;
    const rgb24 statusColor = dstActive ? rgb24{0xff, 0x90, 0x20} : rgb24{0x40, 0xa0, 0xff};
    backgroundLayer.setFont(font3x5);
    backgroundLayer.drawString(statusX, 26, statusColor, statusText);

    if (showBlinkPixel)
    {
        backgroundLayer.drawPixel(kMatrixWidth - 1, kMatrixHeight - 1, {0xff, 0xff, 0xff});
    }

    backgroundLayer.swapBuffers(true);
}

/**
 * Initializes USB serial, display layers, RTC access, and SPI slave support.
 */
void setup()
{
    USB_serial.begin(kSerialBaud);

    matrix.addLayer(&backgroundLayer);
    matrix.begin();

    showStartupSplash();
    configureRtcPins();
    configureSpiSlavePins();
    initializeSpiSlave();
    syncTimeFromRtc();

    setupStartMillis = millis();
}

#ifndef PIO_UNIT_TESTING
/**
 * Main runtime loop: services SPI and USB input continuously and refreshes the
 * displayed clock state once per configured interval.
 */
void loop()
{
    const unsigned long now = millis();

    pollSpiTimeFrame();
    processSpiTimeFrame();
    maybeShowStartupPrompt(now);

    if (USB_serial.available() > 0)
    {
        setRTCFromSerial();
    }

    if (now - lastDisplayUpdate < kDisplayUpdateIntervalMs)
    {
        return;
    }

    lastDisplayUpdate = now;

    if (!RTC.read(tm))
    {
        return;
    }

    dst = isDstActive(tm);
    updateUtcTime(tm, dst);

    printClockDisplay(tm, dst);
    printUtcClockDisplay(utcTime);

    drawClockScreen(tm, dst);
}
#endif

 /******************************************************************************
 * Project     : Display Matrix Clock
 * File        : main.c 
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
 ******************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <Time.h>
#include <DS1307RTC.h>
// SmartMatrix output disabled in this branch; keep the original include
// commented for easy reactivation later.
// #include <SmartMatrix3.h>

namespace
{
// SmartMatrix output disabled in this branch; keep the original display
// configuration commented for easy reactivation later.
// #define COLOR_DEPTH 24
// constexpr uint8_t kMatrixWidth = 32;
// constexpr uint8_t kMatrixHeight = 32;
// constexpr uint8_t kRefreshDepth = 48;
// constexpr uint8_t kDmaBufferRows = 4;
// constexpr uint8_t kPanelType = SMARTMATRIX_HUB75_32ROW_MOD16SCAN;
// constexpr uint8_t kMatrixOptions = SMARTMATRIX_OPTIONS_NONE;
// constexpr uint8_t kIndexedLayerOptions = SM_INDEXED_OPTIONS_NONE;
//
// constexpr int kNightBrightness = 0;
// constexpr int kDayBrightness = 10;
//
// constexpr unsigned long kDisplayUpdateIntervalMs = 1000;
constexpr unsigned long kUsbSerialStartupPromptDelayMs = 2000;
// constexpr unsigned long kStartupSplashDurationMs = 5000;
constexpr unsigned long kStatusUpdateIntervalMs = 1000;
constexpr uint32_t kUsbSerialBaud = 9600;
constexpr uint32_t kUartSerialBaud = 115200;

constexpr uint8_t kDstStartMonth = 3;
constexpr uint8_t kDstEndMonth = 10;
constexpr uint8_t kDstStartHour = 2;
constexpr uint8_t kDstEndHour = 3;
// constexpr uint8_t kNightModeStartHour = 20;
// constexpr uint8_t kNightModeEndHour = 8;
constexpr int kStandardUtcOffsetHours = 1;

// constexpr char kStartupLabel[] = "DK1AJ";
constexpr char kRtcInputFormat[] = "YYYY-MM-DD HH:MM:SS + CR/LF";
constexpr char kUsbSerialSourceLabel[] = "USB Serial";
constexpr char kUartSerialSourceLabel[] = "UART Serial";
}

// SmartMatrix output disabled in this branch; keep the original display
// objects commented for easy reactivation later.
// SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
// SMARTMATRIX_ALLOCATE_INDEXED_LAYER(indexedLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

bool dst = true;
unsigned long lastStatusUpdate = 0;
unsigned long usbSerialPromptStartMillis = 0;
bool usbSerialStartupPromptShown = false;
tmElements_t tm;
tmElements_t utcTime;

struct SerialLineInputState
{
    char input[32];
    uint8_t idx = 0;
    bool ignoreNextLineFeed = false;
};

SerialLineInputState usbSerialInputState;
SerialLineInputState uartSerialInputState;

bool isLeapYear(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

uint8_t lastDayOfMonth(int year, int month)
{
    static const uint8_t kDaysInMonth[] =
    {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month == 2 && isLeapYear(year))
    {
        return 29;
    }

    return kDaysInMonth[month - 1];
}

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

        time_t t = makeTime(date);
        breakTime(t, date);

        if (date.Wday == 1)
        {
            return day;
        }
    }

    return lastDay;
}

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

void updateUtcTime(const tmElements_t& rtcTime)
{
    // The RTC is handled as local standard time (CET), so UTC is one hour behind.
    const time_t utcTimestamp = makeTime(rtcTime) - (kStandardUtcOffsetHours * SECS_PER_HOUR);
    breakTime(utcTimestamp, utcTime);
}

const char* dstLabel(bool dstActive)
{
    return dstActive ? "Sommer" : "Winter";
}

void printUsbSerialTwoDigits(int value)
{
    if (value < 10)
    {
        Serial.print('0');
    }

    Serial.print(value);
}

void printClockStatusToUsbSerial(const tmElements_t& rtcTime, bool dstActive)
{
    Serial.print(rtcTime.Hour);
    Serial.print(':');
    printUsbSerialTwoDigits(rtcTime.Minute);
    Serial.print(':');
    printUsbSerialTwoDigits(rtcTime.Second);
    Serial.print(' ');
    Serial.print(rtcTime.Day);
    Serial.print('.');
    Serial.print(rtcTime.Month);
    Serial.print('.');
    Serial.print(tmYearToCalendar(rtcTime.Year));
    Serial.print(' ');
    Serial.print(dstLabel(dstActive));
    Serial.print(" wday=");
    Serial.println(rtcTime.Wday);
}

void printUtcClockStatusToUsbSerial(const tmElements_t& timeUtc)
{
    Serial.print("UTC ");
    Serial.print(timeUtc.Hour);
    Serial.print(':');
    printUsbSerialTwoDigits(timeUtc.Minute);
    Serial.print(':');
    printUsbSerialTwoDigits(timeUtc.Second);
    Serial.print(' ');
    Serial.print(timeUtc.Day);
    Serial.print('.');
    Serial.print(timeUtc.Month);
    Serial.print('.');
    Serial.println(tmYearToCalendar(timeUtc.Year));
}

void printRtcUpdateTimestampToUsbSerial(int year, int month, int day, int hour, int minute, int second)
{
    Serial.print("RTC set to: ");
    Serial.print(year);
    Serial.print('-');
    printUsbSerialTwoDigits(month);
    Serial.print('-');
    printUsbSerialTwoDigits(day);
    Serial.print(' ');
    printUsbSerialTwoDigits(hour);
    Serial.print(':');
    printUsbSerialTwoDigits(minute);
    Serial.print(':');
    printUsbSerialTwoDigits(second);
    Serial.println();
}

void printRtcInputSourceToUsbSerial(const char* serialSourceLabel)
{
    Serial.print("RTC input via ");
    Serial.println(serialSourceLabel);
}

bool hasBasicDateTimeRange(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 2000 && year <= 2099 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

bool parseRtcDateTimeInput(const char* input, tmElements_t& normalized, time_t& parsedTime)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(input, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        Serial.print("Received: [");
        Serial.print(input);
        Serial.println(']');
        Serial.print("Invalid format. Please enter: ");
        Serial.println(kRtcInputFormat);
        return false;
    }

    if (!hasBasicDateTimeRange(year, month, day, hour, minute, second))
    {
        Serial.println("Invalid date/time values");
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
        Serial.println("Invalid calendar date");
        return false;
    }

    return true;
}

void discardRemainingSerialLine(Stream& serialStream)
{
    while (serialStream.available() > 0)
    {
        if (serialStream.read() == '\n')
        {
            break;
        }
    }
}

bool applyRtcUpdateFromSerialInput(tmElements_t normalized, time_t parsedTime, const char* serialSourceLabel)
{
    if (!RTC.write(normalized))
    {
        Serial.println("Error: RTC write failed");
        return false;
    }

    setTime(parsedTime);
    printRtcInputSourceToUsbSerial(serialSourceLabel);
    printRtcUpdateTimestampToUsbSerial(
        tmYearToCalendar(normalized.Year),
        normalized.Month,
        normalized.Day,
        normalized.Hour,
        normalized.Minute,
        normalized.Second);
    return true;
}

bool trySetRtcFromSerialStream(Stream& serialStream, SerialLineInputState& serialInputState, const char* serialSourceLabel)
{
    while (serialStream.available() > 0)
    {
        const char c = serialStream.read();

        if (serialInputState.ignoreNextLineFeed && c == '\n')
        {
            serialInputState.ignoreNextLineFeed = false;
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            serialInputState.ignoreNextLineFeed = (c == '\r');

            serialInputState.input[serialInputState.idx] = '\0';
            serialInputState.idx = 0;

            if (serialInputState.input[0] == '\0')
            {
                return false;
            }

            tmElements_t normalized = {};
            time_t parsedTime = 0;

            if (!parseRtcDateTimeInput(serialInputState.input, normalized, parsedTime))
            {
                return false;
            }

            return applyRtcUpdateFromSerialInput(normalized, parsedTime, serialSourceLabel);
        }

        if (c < 32 || c > 126)
        {
            continue;
        }

        if (serialInputState.idx < sizeof(serialInputState.input) - 1)
        {
            serialInputState.input[serialInputState.idx++] = c;
            continue;
        }

        serialInputState.idx = 0;
        discardRemainingSerialLine(serialStream);
        Serial.println("Input too long");
        return false;
    }

    return false;
}

void configureRtcPins(void)
{
    pinMode(18, INPUT);
    pinMode(19, INPUT);
    CORE_PIN16_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
    CORE_PIN17_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
}

// SmartMatrix output disabled in this branch; keep the original startup
// splash code commented for easy reactivation later.
// void showStartupSplash(void)
// {
//     matrix.setBrightness(kDayBrightness);
//     indexedLayer.fillScreen(0);
//     indexedLayer.setFont(gohufont11b);
//     indexedLayer.drawString(0, kMatrixHeight / 2 - 6, 1, kStartupLabel);
//     indexedLayer.swapBuffers(false);
//
//     const unsigned long splashStart = millis();
//     while (millis() - splashStart < kStartupSplashDurationMs)
//     {
//     }
// }

void syncTimeFromRtc(void)
{
    setSyncProvider(RTC.get);

    if (timeStatus() == timeSet)
    {
        Serial.println("RTC has set the system time");
    }
    else
    {
        Serial.println("Unable to sync with the RTC");
    }
}

void maybeShowUsbSerialStartupPrompt(unsigned long now)
{
    if (usbSerialStartupPromptShown || (now - usbSerialPromptStartMillis < kUsbSerialStartupPromptDelayMs))
    {
        return;
    }

    Serial.println("USB Serial debug active");
    Serial.print("Build: ");
    Serial.print(__DATE__);
    Serial.print(' ');
    Serial.println(__TIME__);
    Serial.print("Set time with: ");
    Serial.println(kRtcInputFormat);
    Serial.println("Example: 2026-03-25 14:30:00");
    usbSerialStartupPromptShown = true;
}

// SmartMatrix output disabled in this branch; keep the original display
// rendering code commented for easy reactivation later.
// uint8_t displayHourFrom(const tmElements_t& rtcTime, bool dstActive)
// {
//     return (rtcTime.Hour + (dstActive ? 1 : 0)) % 24;
// }
//
// void applyBrightness(uint8_t displayHour)
// {
//     const bool nightMode = displayHour >= kNightModeStartHour || displayHour < kNightModeEndHour;
//     matrix.setBrightness(nightMode ? kNightBrightness : kDayBrightness);
// }
//
// void drawClockScreen(const tmElements_t& rtcTime, bool dstActive)
// {
//     const uint8_t displayHour = displayHourFrom(rtcTime, dstActive);
//     char timeBuffer[16];
//     char utcBuffer[16];
//     char dateBuffer[16];
//     char statusBuffer[16];
//
//     applyBrightness(displayHour);
//
//     snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d", displayHour, rtcTime.Minute);
//     snprintf(utcBuffer, sizeof(utcBuffer), "%02d:%02d", utcTime.Hour, utcTime.Minute);
//     snprintf(dateBuffer, sizeof(dateBuffer), "%d.%02d", rtcTime.Day, rtcTime.Month);
//     snprintf(statusBuffer, sizeof(statusBuffer), "%s", dstLabel(dstActive));
//
//     if (displayHour < 10)
//     {
//         indexedLayer.setFont(gohufont11b);
//         indexedLayer.drawString(3, 0, 1, timeBuffer);
//     }
//     else
//     {
//         indexedLayer.setFont(gohufont11b);
//         indexedLayer.drawString(1, 0, 1, timeBuffer);
//     }
//
//     indexedLayer.setFont(font3x5);
//     indexedLayer.drawString(6, 13, 1, utcBuffer);
//     indexedLayer.drawString(6, 19, 1, dateBuffer);
//     indexedLayer.drawString(4, 26, 1, statusBuffer);
//     indexedLayer.swapBuffers();
// }

void setup()
{
    Serial.begin(kUsbSerialBaud);
    Serial1.begin(kUartSerialBaud);

    // SmartMatrix output disabled in this branch; keep the original
    // initialization commented for easy reactivation later.
    // matrix.addLayer(&indexedLayer);
    // matrix.begin();

    // showStartupSplash();
    configureRtcPins();
    syncTimeFromRtc();

    usbSerialPromptStartMillis = millis();
}

#ifndef PIO_UNIT_TESTING
void loop()
{
    const unsigned long now = millis();

    maybeShowUsbSerialStartupPrompt(now);

    if (Serial.available() > 0)
    {
        trySetRtcFromSerialStream(Serial, usbSerialInputState, kUsbSerialSourceLabel);
    }

    if (Serial1.available() > 0)
    {
        trySetRtcFromSerialStream(Serial1, uartSerialInputState, kUartSerialSourceLabel);
    }

    if (now - lastStatusUpdate < kStatusUpdateIntervalMs)
    {
        return;
    }

    lastStatusUpdate = now;

    // SmartMatrix output disabled in this branch; keep the original clear
    // operation commented for easy reactivation later.
    // indexedLayer.fillScreen(0);

    if (!RTC.read(tm))
    {
        return;
    }

    updateUtcTime(tm);
    dst = isDstActive(tm);

    printClockStatusToUsbSerial(tm, dst);
    printUtcClockStatusToUsbSerial(utcTime);

    // drawClockScreen(tm, dst);
}
#endif

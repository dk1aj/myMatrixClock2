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
#include <SmartMatrix3.h>


#define COLOR_DEPTH 24

namespace
{
constexpr uint8_t kMatrixWidth = 32;
constexpr uint8_t kMatrixHeight = 32;
constexpr uint8_t kRefreshDepth = 48;
constexpr uint8_t kDmaBufferRows = 4;
constexpr uint8_t kPanelType = SMARTMATRIX_HUB75_32ROW_MOD16SCAN;
constexpr uint8_t kMatrixOptions = SMARTMATRIX_OPTIONS_NONE;
constexpr uint8_t kIndexedLayerOptions = SM_INDEXED_OPTIONS_NONE;
constexpr uint8_t kVisibleTextColorIndex = 1;
constexpr uint8_t kLocalTimeWhiteLevel = 255;
constexpr uint8_t kDateWhiteLevel = 170;
constexpr uint8_t kFooterWhiteLevel = 210;
constexpr uint8_t kUtcDayRedLevel = 230;
constexpr uint8_t kUtcNightRedLevel = 255;
constexpr uint8_t kUtcZYellowLevel = 220;
constexpr uint8_t kWeekdayBlueLevel = 255;

constexpr int kNightBrightness = 0;
constexpr int kDayBrightness = 10;
constexpr int kLedPin = 13;

constexpr unsigned long kDisplayUpdateIntervalMs = 1000;
constexpr unsigned long kStartupPromptDelayMs = 2000;
constexpr unsigned long kStartupSplashDurationMs = 3000;
constexpr uint32_t kSerialBaud = 9600;

constexpr uint8_t kDstStartMonth = 3;
constexpr uint8_t kDstEndMonth = 10;
constexpr uint8_t kDstStartHour = 2;
constexpr uint8_t kDstEndHour = 3;
constexpr uint8_t kNightModeStartHour = 20;
constexpr uint8_t kNightModeEndHour = 8;
constexpr int kStandardUtcOffsetHours = 1;

constexpr char kStartupLabel[] = "DK1AJ";
constexpr char kTimeInputFormat[] = "YYYY-MM-DD HH:MM:SS + CR/LF";
}

SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(localTimeLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(dateLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(footerLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(utcLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(zLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(weekdayLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

bool dst = true;
unsigned long lastDisplayUpdate = 0;
unsigned long setupStartMillis = 0;
bool startupMessageShown = false;
tmElements_t tm;
tmElements_t utcTime;

struct LocalizedTextTable
{
    const char* summerLabel;
    const char* winterLabel;
    const char* weekdayLabels[8];
};

constexpr LocalizedTextTable kLocalizedTextTables[] =
{
    {
        "Sommer",
        "Winter",
        {"", "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donners.", "Freitag", "Samstag"}
    },
    {
        "Summer",
        "Winter",
        {"", "Sunday", "Monday", "Tuesday", "Wednes.", "Thursday", "Friday", "Saturday"}
    },
    {
        "Vara",
        "Iarna",
        {"", "Duminica", "Luni", "Marti", "Miercuri", "Joi", "Vineri", "Sambata"}
    }
};

/* Returns whether the supplied calendar year is a leap year. */
bool isLeapYear(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

/* Returns the last valid day number for the given month and year. */
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

/* Finds the calendar day of the last Sunday in the given month. */
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

/* Determines whether daylight saving time is active for the given local time. */
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

/* Converts the current local RTC time into UTC using the active seasonal offset. */
void updateUtcTime(const tmElements_t& rtcTime, bool dstActive)
{
    /* The RTC is handled as local civil time, so UTC depends on the active
       seasonal offset. */
    const int localUtcOffsetHours = kStandardUtcOffsetHours + (dstActive ? 1 : 0);
    const time_t utcTimestamp = makeTime(rtcTime) - (localUtcOffsetHours * SECS_PER_HOUR);
    breakTime(utcTimestamp, utcTime);
}

/* Selects the active translation table based on the current 10-second display slot. */
const LocalizedTextTable& localizedTextTableForSecond(uint8_t second)
{
    const uint8_t languageSlot = ((second / 10) / 2) % (sizeof(kLocalizedTextTables) / sizeof(kLocalizedTextTables[0]));
    return kLocalizedTextTables[languageSlot];
}

/* Returns the localized summer or winter label for the active language slot. */
const char* dstLabel(bool dstActive, const LocalizedTextTable& localizedTextTable)
{
    return dstActive ? localizedTextTable.summerLabel : localizedTextTable.winterLabel;
}

/* Returns the localized weekday label for the active language slot. */
const char* weekdayLabel(uint8_t weekday, const LocalizedTextTable& localizedTextTable)
{
    if (weekday >= 1 && weekday <= 7)
    {
        return localizedTextTable.weekdayLabels[weekday];
    }

    return "---";
}

/* Calculates a centered X position for font3x5 text on the 32-pixel-wide display. */
int16_t centeredFont3x5X(const char* text)
{
    uint8_t textLength = 0;

    while (text[textLength] != '\0')
    {
        ++textLength;
    }

    constexpr int16_t kFont3x5CharWidth = 4;
    const int16_t textWidth = textLength * kFont3x5CharWidth;
    const int16_t centeredX = (kMatrixWidth - textWidth) / 2;
    return centeredX < 0 ? 0 : centeredX;
}

/* Prints a decimal value to Serial with a leading zero when needed. */
void printTwoDigits(int value)
{
    if (value < 10)
    {
        Serial.print('0');
    }

    Serial.print(value);
}

/* Writes the current local clock state to the serial debug output. */
void printClockDisplay(const tmElements_t& rtcTime, bool dstActive)
{
    const LocalizedTextTable& germanTextTable = kLocalizedTextTables[0];

    Serial.print(rtcTime.Hour);
    Serial.print(':');
    printTwoDigits(rtcTime.Minute);
    Serial.print(':');
    printTwoDigits(rtcTime.Second);
    Serial.print(' ');
    Serial.print(rtcTime.Day);
    Serial.print('.');
    Serial.print(rtcTime.Month);
    Serial.print('.');
    Serial.print(tmYearToCalendar(rtcTime.Year));
    Serial.print(' ');
    Serial.print(dstLabel(dstActive, germanTextTable));
    Serial.print(" wday=");
    Serial.println(rtcTime.Wday);
}

/* Writes the current UTC clock state to the serial debug output. */
void printUtcClockDisplay(const tmElements_t& timeUtc)
{
    Serial.print("UTC ");
    Serial.print(timeUtc.Hour);
    Serial.print(':');
    printTwoDigits(timeUtc.Minute);
    Serial.print(':');
    printTwoDigits(timeUtc.Second);
    Serial.print(' ');
    Serial.print(timeUtc.Day);
    Serial.print('.');
    Serial.print(timeUtc.Month);
    Serial.print('.');
    Serial.println(tmYearToCalendar(timeUtc.Year));
}

/* Prints the normalized timestamp after a successful RTC update. */
void printNormalizedTimestamp(int year, int month, int day, int hour, int minute, int second)
{
    Serial.print("RTC set to: ");
    Serial.print(year);
    Serial.print('-');
    printTwoDigits(month);
    Serial.print('-');
    printTwoDigits(day);
    Serial.print(' ');
    printTwoDigits(hour);
    Serial.print(':');
    printTwoDigits(minute);
    Serial.print(':');
    printTwoDigits(second);
    Serial.println();
}

/* Performs a basic range check before a date-time string is normalized. */
bool hasBasicDateTimeRange(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 2000 && year <= 2099 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

/* Parses a serial date-time string and normalizes it through TimeLib. */
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
        Serial.print("Received: [");
        Serial.print(input);
        Serial.println(']');
        Serial.print("Invalid format. Please enter: ");
        Serial.println(kTimeInputFormat);
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

/* Drops the remaining characters of the current serial line after an input error. */
void discardSerialLine(void)
{
    while (Serial.available() > 0)
    {
        if (Serial.read() == '\n')
        {
            break;
        }
    }
}

/* Reads a timestamp from Serial and writes it into the RTC when complete. */
bool setRTCFromSerial(void)
{
    static char input[32];
    static uint8_t idx = 0;
    static bool ignoreNextLineFeed = false;

    while (Serial.available() > 0)
    {
        const char c = Serial.read();

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

            tmElements_t normalized = {};
            time_t parsedTime = 0;

            if (!parseRtcInput(input, normalized, parsedTime))
            {
                return false;
            }

            if (!RTC.write(normalized))
            {
                Serial.println("Error: RTC write failed");
                return false;
            }

            setTime(parsedTime);
            printNormalizedTimestamp(
                tmYearToCalendar(normalized.Year),
                normalized.Month,
                normalized.Day,
                normalized.Hour,
                normalized.Minute,
                normalized.Second);
            return true;
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
        Serial.println("Input too long");
        return false;
    }

    return false;
}

/* Reconfigures the Teensy pins so the RTC uses the SmartMatrix-compatible I2C pins. */
void configureRtcPins(void)
{
    Wire.setSCL(16);
    Wire.setSDA(17);
    Wire.begin();
}

/* Displays the startup splash screen before the regular clock loop begins. */
void showStartupSplash(void)
{
    matrix.setBrightness(kDayBrightness);
    localTimeLayer.fillScreen(0);
    localTimeLayer.setFont(gohufont11b);
    localTimeLayer.drawString(0, kMatrixHeight / 2 - 6, 1, kStartupLabel);
    localTimeLayer.swapBuffers(false);

    const unsigned long splashStart = millis();
    while (millis() - splashStart < kStartupSplashDurationMs)
    {
    }
}

/* Uses the RTC as the TimeLib synchronization source. */
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

/* Prints the serial usage hint once shortly after startup. */
void maybeShowStartupPrompt(unsigned long now)
{
    if (startupMessageShown || (now - setupStartMillis < kStartupPromptDelayMs))
    {
        return;
    }

    Serial.print("Set time with: ");
    Serial.println(kTimeInputFormat);
    Serial.println("Example: 2026-03-25 14:30:00");
    startupMessageShown = true;
}

/* Returns the hour value that should be shown on the main local-time display. */
uint8_t displayHourFrom(const tmElements_t& rtcTime, bool dstActive)
{
    (void)dstActive;
    return rtcTime.Hour;
}

/* Applies day or night brightness according to the currently displayed hour. */
void applyBrightness(uint8_t displayHour)
{
    const bool nightMode = displayHour >= kNightModeStartHour || displayHour < kNightModeEndHour;
    matrix.setBrightness(nightMode ? kNightBrightness : kDayBrightness);
}

/* Updates indexed-layer colors that need to react to the current night mode. */
void applyLayerColors(uint8_t displayHour)
{
    const bool nightMode = displayHour >= kNightModeStartHour || displayHour < kNightModeEndHour;

    localTimeLayer.setIndexedColor(
        kVisibleTextColorIndex,
        {kLocalTimeWhiteLevel, kLocalTimeWhiteLevel, kLocalTimeWhiteLevel});
    dateLayer.setIndexedColor(
        kVisibleTextColorIndex,
        {kDateWhiteLevel, kDateWhiteLevel, kDateWhiteLevel});
    footerLayer.setIndexedColor(
        kVisibleTextColorIndex,
        {kFooterWhiteLevel, kFooterWhiteLevel, kFooterWhiteLevel});
    utcLayer.setIndexedColor(
        kVisibleTextColorIndex,
        {nightMode ? kUtcNightRedLevel : kUtcDayRedLevel, 0x00, 0x00});
    zLayer.setIndexedColor(kVisibleTextColorIndex, {kUtcZYellowLevel, kUtcZYellowLevel, 0x00});
    weekdayLayer.setIndexedColor(kVisibleTextColorIndex, {0x00, 0x00, kWeekdayBlueLevel});
}

/* Draws the complete clock frame, including localized footer text and colored UTC layers. */
void drawClockScreen(const tmElements_t& rtcTime, bool dstActive)
{
    const uint8_t displayHour = displayHourFrom(rtcTime, dstActive);
    char timeBuffer[16];
    char utcTimeBuffer[16];
    char dateBuffer[16];
    char footerBuffer[16];
    int16_t footerX = 4;
    const int16_t utcTimeX = centeredFont3x5X("00:00 Z");
    const int16_t utcZX = utcTimeX + (6 * 4);
    constexpr char kUtcZLabel[] = "Z";
    const LocalizedTextTable& localizedTextTable = localizedTextTableForSecond(rtcTime.Second);

    applyBrightness(displayHour);
    applyLayerColors(displayHour);

    snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d", displayHour, rtcTime.Minute);
    snprintf(utcTimeBuffer, sizeof(utcTimeBuffer), "%02d:%02d ", utcTime.Hour, utcTime.Minute);
    snprintf(dateBuffer, sizeof(dateBuffer), "%d.%02d", rtcTime.Day, rtcTime.Month);

    const bool showDstLabel = ((rtcTime.Second / 10) % 2) == 0;
    snprintf(
        footerBuffer,
        sizeof(footerBuffer),
        "%s",
        showDstLabel ? dstLabel(dstActive, localizedTextTable) : weekdayLabel(rtcTime.Wday, localizedTextTable));
    footerX = centeredFont3x5X(footerBuffer);

    if (displayHour < 10)
    {
        localTimeLayer.setFont(gohufont11b);
        localTimeLayer.drawString(3, 0, kVisibleTextColorIndex, timeBuffer);
    }
    else
    {
        localTimeLayer.setFont(gohufont11b);
        localTimeLayer.drawString(1, 0, kVisibleTextColorIndex, timeBuffer);
    }

    dateLayer.setFont(font3x5);
    dateLayer.drawString(6, 19, kVisibleTextColorIndex, dateBuffer);
    utcLayer.setFont(font3x5);
    utcLayer.drawString(utcTimeX, 11, kVisibleTextColorIndex, utcTimeBuffer);
    zLayer.setFont(font3x5);
    zLayer.drawString(utcZX, 11, kVisibleTextColorIndex, kUtcZLabel);

    if (showDstLabel)
    {
        footerLayer.setFont(font3x5);
        footerLayer.drawString(footerX, 26, kVisibleTextColorIndex, footerBuffer);
    }
    else
    {
        weekdayLayer.setFont(font3x5);
        weekdayLayer.drawString(footerX, 26, kVisibleTextColorIndex, footerBuffer);
    }

    localTimeLayer.swapBuffers();
    dateLayer.swapBuffers();
    footerLayer.swapBuffers();
    utcLayer.swapBuffers();
    zLayer.swapBuffers();
    weekdayLayer.swapBuffers();
}

/* Initializes serial, display layers, RTC pin routing, and startup state. */
void setup()
{
    pinMode(kLedPin, OUTPUT);
    Serial.begin(kSerialBaud);

    matrix.addLayer(&localTimeLayer);
    matrix.addLayer(&dateLayer);
    matrix.addLayer(&footerLayer);
    matrix.addLayer(&utcLayer);
    matrix.addLayer(&zLayer);
    matrix.addLayer(&weekdayLayer);
    matrix.begin();
    applyLayerColors(12);

    showStartupSplash();
    configureRtcPins();
    syncTimeFromRtc();

    setupStartMillis = millis();
}

#ifndef PIO_UNIT_TESTING
/* Polls serial input, refreshes the RTC state, and updates the display once per second. */
void loop()
{
    const unsigned long now = millis();

    maybeShowStartupPrompt(now);

    if (Serial.available() > 0)
    {
        setRTCFromSerial();
    }

    if (now - lastDisplayUpdate < kDisplayUpdateIntervalMs)
    {
        return;
    }

    lastDisplayUpdate = now;
    localTimeLayer.fillScreen(0);
    dateLayer.fillScreen(0);
    footerLayer.fillScreen(0);
    utcLayer.fillScreen(0);
    zLayer.fillScreen(0);
    weekdayLayer.fillScreen(0);

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

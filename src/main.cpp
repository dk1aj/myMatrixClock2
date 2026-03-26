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

constexpr int kNightBrightness = 0;
constexpr int kDayBrightness = 10;
constexpr int kLedPin = 13;

constexpr unsigned long kDisplayUpdateIntervalMs = 1000;
constexpr unsigned long kStartupPromptDelayMs = 2000;
constexpr unsigned long kStartupSplashDurationMs = 5000;
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
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(indexedLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

bool dst = true;
unsigned long lastDisplayUpdate = 0;
unsigned long setupStartMillis = 0;
bool startupMessageShown = false;
tmElements_t tm;
tmElements_t utcTime;

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

void printTwoDigits(int value)
{
    if (value < 10)
    {
        Serial.print('0');
    }

    Serial.print(value);
}

void printClockDisplay(const tmElements_t& rtcTime, bool dstActive)
{
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
    Serial.print(dstLabel(dstActive));
    Serial.print(" wday=");
    Serial.println(rtcTime.Wday);
}

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

bool hasBasicDateTimeRange(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 2000 && year <= 2099 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

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

        if (c == '\r' || c == '\n')
        {
            ignoreNextLineFeed = (c == '\r');

            input[idx] = '\0';
            idx = 0;

            if (input[0] == '\0')
            {
                return false;
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

void configureRtcPins(void)
{
    pinMode(18, INPUT);
    pinMode(19, INPUT);
    CORE_PIN16_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
    CORE_PIN17_CONFIG = PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS;
}

void showStartupSplash(void)
{
    matrix.setBrightness(kDayBrightness);
    indexedLayer.fillScreen(0);
    indexedLayer.setFont(gohufont11b);
    indexedLayer.drawString(0, kMatrixHeight / 2 - 6, 1, kStartupLabel);
    indexedLayer.swapBuffers(false);

    const unsigned long splashStart = millis();
    while (millis() - splashStart < kStartupSplashDurationMs)
    {
    }
}

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

uint8_t displayHourFrom(const tmElements_t& rtcTime, bool dstActive)
{
    return (rtcTime.Hour + (dstActive ? 1 : 0)) % 24;
}

void applyBrightness(uint8_t displayHour)
{
    const bool nightMode = displayHour >= kNightModeStartHour || displayHour < kNightModeEndHour;
    matrix.setBrightness(nightMode ? kNightBrightness : kDayBrightness);
}

void drawClockScreen(const tmElements_t& rtcTime, bool dstActive)
{
    const uint8_t displayHour = displayHourFrom(rtcTime, dstActive);
    char timeBuffer[16];
    char utcBuffer[16];
    char dateBuffer[16];
    char statusBuffer[16];

    applyBrightness(displayHour);

    snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d", displayHour, rtcTime.Minute);
    snprintf(utcBuffer, sizeof(utcBuffer), "%02d:%02d", utcTime.Hour, utcTime.Minute);
    snprintf(dateBuffer, sizeof(dateBuffer), "%d.%02d", rtcTime.Day, rtcTime.Month);
    snprintf(statusBuffer, sizeof(statusBuffer), "%s", dstLabel(dstActive));

    if (displayHour < 10)
    {
        indexedLayer.setFont(gohufont11b);
        indexedLayer.drawString(3, 0, 1, timeBuffer);
    }
    else
    {
        indexedLayer.setFont(gohufont11b);
        indexedLayer.drawString(1, 0, 1, timeBuffer);
    }

    indexedLayer.setFont(font3x5);
    indexedLayer.drawString(6, 13, 1, utcBuffer);
    indexedLayer.drawString(6, 19, 1, dateBuffer);
    indexedLayer.drawString(4, 26, 1, statusBuffer);
    indexedLayer.swapBuffers();
}

void setup()
{
    pinMode(kLedPin, OUTPUT);
    Serial.begin(kSerialBaud);

    matrix.addLayer(&indexedLayer);
    matrix.begin();

    showStartupSplash();
    configureRtcPins();
    syncTimeFromRtc();

    setupStartMillis = millis();
}

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
    indexedLayer.fillScreen(0);

    if (!RTC.read(tm))
    {
        return;
    }

    updateUtcTime(tm);
    dst = isDstActive(tm);

    printClockDisplay(tm, dst);
    printUtcClockDisplay(utcTime);

    drawClockScreen(tm, dst);
}

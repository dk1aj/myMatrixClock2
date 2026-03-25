#include <Arduino.h>
#include <Wire.h>
#include <Time.h>
#include <DS1307RTC.h>
#include <SmartMatrix3.h>

#define DEBUG_LDR 1
#define COLOR_DEPTH 24

const uint8_t kMatrixWidth = 32;
const uint8_t kMatrixHeight = 32;
const uint8_t kRefreshDepth = 48;
const uint8_t kDmaBufferRows = 4;
const uint8_t kPanelType = SMARTMATRIX_HUB75_32ROW_MOD16SCAN;
const uint8_t kMatrixOptions = (SMARTMATRIX_OPTIONS_NONE);
const uint8_t kIndexedLayerOptions = (SM_INDEXED_OPTIONS_NONE);

SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(indexedLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

const int nightBrightness = 1;
const int dayBrightness = 10;
const int ledPin = 13;

boolean dst = 1;

unsigned long lastDisplayUpdate = 0;
unsigned long setupStartMillis = 0;
bool startupMessageShown = false;

tmElements_t tm;

/*
 * Calculate whether daylight saving time is currently active.
 *
 * This logic follows the common Central European DST rule:
 * - DST is inactive from January through February and from November through December.
 * - DST is active from April through September.
 * - In March, DST starts on the last Sunday at 02:00.
 * - In October, DST ends on the last Sunday at 03:00.
 *
 * The code uses tm.Day and tm.Wday from the RTC time structure.
 * In the Time library, Wday is 1 for Sunday.
 */
/*
 * Return the calendar day number of the last Sunday in a given month.
 *
 * Parameters:
 * - year: full calendar year, for example 2026
 * - month: 1..12
 *
 * The function checks the last seven days of the month and returns
 * the day number that falls on Sunday.
 */
uint8_t lastSundayOfMonth(int year, int month)
{
    static const uint8_t daysInMonth[] =
    {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    uint8_t lastDay = daysInMonth[month - 1];

    /*
     * Leap year correction for February.
     */
    if (month == 2)
    {
        bool leapYear = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        if (leapYear)
        {
            lastDay = 29;
        }
    }

    /*
     * Start from the last day of the month and walk backwards
     * until a Sunday is found.
     */
    for (int day = lastDay; day >= lastDay - 6; day--)
    {
        tmElements_t temp;
        temp.Year   = CalendarYrToTm(year);
        temp.Month  = month;
        temp.Day    = day;
        temp.Hour   = 12;
        temp.Minute = 0;
        temp.Second = 0;

        time_t t = makeTime(temp);
        breakTime(t, temp);

        if (temp.Wday == 1)
        {
            return day;
        }
    }

    return 31;
}

/*
 * Calculate whether daylight saving time is active.
 *
 * Base time in the RTC is always standard time (CET / winter time).
 *
 * DST rules used here:
 * - January, February, November, December: DST off
 * - April to September: DST on
 * - March: DST starts on the last Sunday at 02:00 CET
 * - October: DST ends on the last Sunday at 03:00 CET
 */
void my_DaylightSavingTime(void)
{
    int year = tmYearToCalendar(tm.Year);

    if (tm.Month < 3 || tm.Month > 10)
    {
        dst = 0;
        return;
    }

    if (tm.Month > 3 && tm.Month < 10)
    {
        dst = 1;
        return;
    }

    if (tm.Month == 3)
    {
        uint8_t changeDay = lastSundayOfMonth(year, 3);

        if (tm.Day < changeDay)
        {
            dst = 0;
        }
        else if (tm.Day > changeDay)
        {
            dst = 1;
        }
        else
        {
            dst = (tm.Hour >= 2) ? 1 : 0;
        }

        return;
    }

    if (tm.Month == 10)
    {
        uint8_t changeDay = lastSundayOfMonth(year, 10);

        if (tm.Day < changeDay)
        {
            dst = 1;
        }
        else if (tm.Day > changeDay)
        {
            dst = 0;
        }
        else
        {
            dst = (tm.Hour >= 3) ? 0 : 1;
        }

        return;
    }
}
/*
 * Print clock-style minutes or seconds to the serial interface.
 *
 * This function always prints a colon first and then the value,
 * adding a leading zero for single-digit numbers.
 *
 * Example:
 * input 7  -> ":07"
 * input 42 -> ":42"
 */
void printDigits(int16_t digits)
{
    Serial.print(":");

    if (digits < 10)
    {
        Serial.print('0');
    }

    Serial.print(digits);
}

/*
 * Read one full line from the serial monitor and try to set the RTC.
 *
 * Expected input format:
 * YYYY-MM-DD HH:MM:SS
 *
 * Example:
 * 2026-03-25 14:30:00
 *
 * Behavior:
 * - Ignores carriage return characters.
 * - Waits for a newline to treat the line as complete.
 * - Accepts only printable characters into the buffer.
 * - Rejects overlong input safely.
 * - Validates numeric ranges before writing to the RTC.
 * - Calculates the weekday automatically from the entered date.
 * - Updates both the RTC and the system time if successful.
 */
bool setRTCFromSerial(void)
{
    static char input[32];
    static uint8_t idx = 0;

    while (Serial.available() > 0)
    {
        char c = Serial.read();

        if (c == '\r')
        {
            continue;
        }

        if (c == '\n')
        {
            input[idx] = '\0';

            if (idx == 0)
            {
                return false;
            }

            idx = 0;

            int year, month, day, hour, minute, second;

            if (sscanf(input, "%d-%d-%d %d:%d:%d",
                       &year, &month, &day,
                       &hour, &minute, &second) == 6)
            {
                if (year < 2000 || year > 2099 ||
                    month < 1 || month > 12 ||
                    day < 1 || day > 31 ||
                    hour < 0 || hour > 23 ||
                    minute < 0 || minute > 59 ||
                    second < 0 || second > 59)
                {
                    Serial.println("Invalid date/time values");
                    return false;
                }

                tmElements_t tmSet;

                /*
                 * The Time library stores the year as an internal offset.
                 * CalendarYrToTm converts a normal calendar year such as 2026
                 * into the format expected by tmElements_t.
                 */
                tmSet.Year   = CalendarYrToTm(year);
                tmSet.Month  = month;
                tmSet.Day    = day;
                tmSet.Hour   = hour;
                tmSet.Minute = minute;
                tmSet.Second = second;

                /*
                 * Convert the entered time to time_t and then back to
                 * tmElements_t so that the weekday field is filled correctly.
                 */
                time_t t = makeTime(tmSet);
                breakTime(t, tmSet);

                if (RTC.write(tmSet))
                {
                    /*
                     * Keep the software clock synchronized with the RTC value
                     * that has just been written.
                     */
                    setTime(t);

                    Serial.print("RTC set to: ");
                    Serial.print(year);
                    Serial.print("-");
                    if (month < 10) Serial.print("0");
                    Serial.print(month);
                    Serial.print("-");
                    if (day < 10) Serial.print("0");
                    Serial.print(day);
                    Serial.print(" ");
                    if (hour < 10) Serial.print("0");
                    Serial.print(hour);
                    Serial.print(":");
                    if (minute < 10) Serial.print("0");
                    Serial.print(minute);
                    Serial.print(":");
                    if (second < 10) Serial.print("0");
                    Serial.println(second);

                    return true;
                }
                else
                {
                    Serial.println("Error: RTC write failed");
                    return false;
                }
            }
            else
            {
                /*
                 * Echo the received line for diagnostics.
                 * This helps identify malformed or unexpected input.
                 */
                Serial.print("Received: [");
                Serial.print(input);
                Serial.println("]");
                Serial.println("Invalid format. Please enter: YYYY-MM-DD HH:MM:SS");
                return false;
            }
        }

        /*
         * Accept only printable ASCII characters.
         * This prevents stray control characters from corrupting the buffer.
         */
        if (c >= 32 && c <= 126)
        {
            if (idx < sizeof(input) - 1)
            {
                input[idx++] = c;
            }
            else
            {
                /*
                 * If the input is too long, discard the rest of the line
                 * so the parser starts cleanly on the next attempt.
                 */
                idx = 0;

                while (Serial.available() > 0)
                {
                    char dump = Serial.read();
                    if (dump == '\n')
                    {
                        break;
                    }
                }

                Serial.println("Input too long");
                return false;
            }
        }
    }

    return false;
}

/*
 * Print the currently read RTC time to the serial port.
 *
 * The year is converted back to a normal calendar year because
 * tm.Year is stored internally as an offset.
 */
static void Serial_Print_Clock_Display(void)
{
    Serial.print(tm.Hour);
    printDigits(tm.Minute);
    printDigits(tm.Second);
    Serial.print(" ");
    Serial.print(tm.Day);
    Serial.print(".");
    Serial.print(tm.Month);
    Serial.print(".");
    Serial.print(tmYearToCalendar(tm.Year));
    Serial.print(" DST=");
    Serial.print(dst);
    Serial.print(" wday=");
    Serial.println(tm.Wday);
}

/*
 * Initialize serial communication, SmartMatrix, alternate I2C routing,
 * and synchronize the software clock with the RTC.
 */
void setup()
{
    pinMode(ledPin, OUTPUT);
    Serial.begin(9600);

    matrix.addLayer(&indexedLayer);
    matrix.begin();

    /*
     * Draw a startup message so something is visible immediately,
     * even if the RTC later fails to respond.
     */
    
	matrix.setBrightness(dayBrightness);
	indexedLayer.fillScreen(0);
    indexedLayer.setFont(gohufont11b);
    indexedLayer.drawString(0, kMatrixHeight / 2 - 6, 1, "DK1AJ");
    indexedLayer.swapBuffers(false);

    matrix.setBrightness(dayBrightness);

	 /*
     * Wait 5 seconds without using delay().
     */
    unsigned long brightnessWaitStart = millis();
    while (millis() - brightnessWaitStart < 5000)
    {
        /*
         * Keep the loop empty on purpose.
         */
    }
	/*
     * Reconfigure the pins so the RTC can use the alternate I2C pins
     * required by this hardware arrangement.
     */
    pinMode(18, INPUT);
    pinMode(19, INPUT);
    CORE_PIN16_CONFIG = (PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS);
    CORE_PIN17_CONFIG = (PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS);

    /*
     * Tell the Time library to use the RTC as its synchronization source.
     */
    setSyncProvider(RTC.get);

    if (timeStatus() != timeSet)
    {
        Serial.println("Unable to sync with the RTC");
    }
    else
    {
        Serial.println("RTC has set the system time");
    }

    setupStartMillis = millis();
}

/*
 * Main loop.
 *
 * Design:
 * - Serial input is handled without blocking delays.
 * - The display is updated once per second using millis().
 * - This keeps the serial interface responsive while maintaining
 *   a steady display refresh rate.
 */
void loop()
{
    /*
     * Show the input format message once, two seconds after startup.
     * This replaces the old blocking delay-based startup behavior.
     */
    if (!startupMessageShown && (millis() - setupStartMillis >= 2000))
    {
        Serial.println("Set time with: YYYY-MM-DD HH:MM:SS");
        Serial.println("Example: 2026-03-25 14:30:00");
        startupMessageShown = true;
    }

    /*
     * Process incoming serial data as soon as it arrives.
     */
    if (Serial.available())
    {
        setRTCFromSerial();
    }

    unsigned long currentMillis = millis();

    /*
     * Update the display once every 1000 milliseconds.
     */
    if (currentMillis - lastDisplayUpdate >= 1000)
    {
        lastDisplayUpdate = currentMillis;

        char timeBuffer[16];
        char txtBuffer[16];
        char dateBuffer[16];

        /*
         * Clear the indexed layer before drawing the new frame.
         */
        indexedLayer.fillScreen(0);

        /*
         * Read the current time from the RTC.
         * Update the display only if the read succeeds.
         */
        if (RTC.read(tm))
        {
            my_DaylightSavingTime();

#ifdef DEBUG_LDR
            Serial_Print_Clock_Display();
#endif

            /*
             * Apply the daylight saving offset for display purposes only.
             * The RTC itself remains in base time.
             */
            uint8_t hour = tm.Hour + dst;
            if (hour > 23)
            {
                hour = 0;
            }

            /*
             * Use low brightness from 20:00 to 07:59,
             * otherwise use normal daytime brightness.
             */
            if (hour >= 20 || hour < 8)
            {
                matrix.setBrightness(nightBrightness);
            }
            else
            {
                matrix.setBrightness(dayBrightness);
            }

            /*
             * Format the display strings.
             * timeBuffer shows hour and minute.
             * dateBuffer shows day and month.
             * txtBuffer shows the DST state.
             */
			// sprintf(timeBuffer, "%d:%02d:%02d", hour, tm.Minute,tm.Second);
            sprintf(timeBuffer, "%d:%02d", hour, tm.Minute);
            sprintf(dateBuffer, "%d.%02d", tm.Day, tm.Month);
            // sprintf(txtBuffer, dst ? "DST 1" : "DST 0");
			sprintf(txtBuffer, "DST %d W%u", dst, tm.Wday);

            /*
             * Draw the time using the larger font.
             * Single-digit hours are shifted slightly right
             * to keep the layout visually balanced.
             */
            // indexedLayer.setFont(font6x10);
			
			
            if (hour < 10)
            {
                indexedLayer.setFont(gohufont11);
				indexedLayer.drawString(3, 0, 1, timeBuffer);
            }
            else
            {
                indexedLayer.setFont(gohufont11b);
				indexedLayer.drawString(1, 0, 1, timeBuffer);
            }

            /*
             * Draw the date and DST indicator using the smaller font.
             */
            indexedLayer.setFont(font3x5);
            indexedLayer.drawString(6, 15, 1, dateBuffer);
            indexedLayer.drawString(0, 27, 1, txtBuffer);

            /*
             * Make the newly drawn frame visible.
             */
            indexedLayer.swapBuffers();
        }
    }
}
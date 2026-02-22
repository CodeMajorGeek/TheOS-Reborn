#include <Device/RTC.h>
#include <Device/CMOS.h>

#include <string.h>

static const char* RTC_DAY_NAMES[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static uint8_t RTC_from_bcd(uint8_t value)
{
    return (uint8_t) ((value & 0x0F) + ((value / 16) * 10));
}

static const char* RTC_weekday_to_name(uint8_t raw_weekday, bool is_bcd)
{
    uint8_t weekday = is_bcd ? RTC_from_bcd(raw_weekday) : raw_weekday;
    if (weekday < 1 || weekday > 7)
        return "Unk";

    // CMOS weekday is 1..7, array index is 0..6.
    return RTC_DAY_NAMES[weekday - 1];
}


bool RTC_update_in_progress(void)
{
    return CMOS_read(RTC_STATUS_REG_A) & RTC_UPDATE_IN_PROGRESS_FLAG;
}

void RTC_wait_ready(void)
{
    while (RTC_update_in_progress())
        __asm__ __volatile__ ("nop");
}

uint8_t RTC_read_status_b(void)
{
    return CMOS_read(RTC_STATUS_REG_B);
}

bool RTC_is_BCD(void)
{
    return !(RTC_read_status_b() & 0x04);
}

uint8_t RTC_read_seconds(void)
{
    return CMOS_read(RTC_SECONDS_REG);
}

uint8_t RTC_read_minutes(void)
{
    return CMOS_read(RTC_MINUTES_REG);
}

uint8_t RTC_read_hours(void)
{
    return CMOS_read(RTC_HOURS_REG);
}

uint8_t RTC_read_weekday(void)
{
    return CMOS_read(RTC_WEEKDAY_REG);
}

uint8_t RTC_read_month_day(void)
{
    return CMOS_read(RTC_MONTH_DAY_REG);
}

uint8_t RTC_read_month(void)
{
    return CMOS_read(RTC_MONTH_REG);
}

uint8_t RTC_read_year(void)
{
    return CMOS_read(RTC_YEAR_REG);
}

uint8_t RTC_read_century(void)
{
    return CMOS_read(RTC_CENTURY_REG);
}

void RTC_read(RTC_t* rtc)
{
    RTC_wait_ready();
    bool is_bcd = RTC_is_BCD();

    rtc->hours = RTC_read_hours();
    rtc->minutes = RTC_read_minutes();
    rtc->seconds = RTC_read_seconds();

    strcpy(rtc->weekday, RTC_weekday_to_name(RTC_read_weekday(), is_bcd));
    rtc->month_day = RTC_read_month_day();
    rtc->month = RTC_read_month();
    rtc->year = RTC_read_year();

    if (is_bcd)
    {
        rtc->hours = RTC_from_bcd(rtc->hours);
        rtc->minutes = RTC_from_bcd(rtc->minutes);
        rtc->seconds = RTC_from_bcd(rtc->seconds);

        rtc->month_day = RTC_from_bcd(rtc->month_day);
        rtc->month = RTC_from_bcd(rtc->month);
        rtc->year = (uint16_t) RTC_from_bcd((uint8_t) rtc->year) + 2000;
    }
}

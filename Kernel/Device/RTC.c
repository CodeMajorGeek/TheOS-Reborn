#include <Device/RTC.h>
#include <Device/CMOS.h>

#include <string.h>

const char* RTC_DAY_NAMES[7] = {
    "Mon\x00", "Tue\x00", "Wed\x00", "Thu\x00", "Fri\x00", "Sat\x00", "Sun\x00"
};


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

    rtc->hours = RTC_read_hours();
    rtc->minutes = RTC_read_minutes();
    rtc->seconds = RTC_read_seconds();

    strcpy(rtc->weekday, RTC_DAY_NAMES[RTC_read_weekday()]);
    rtc->month_day = RTC_read_month_day();
    rtc->month = RTC_read_month();
    rtc->year = RTC_read_year();

    if (RTC_is_BCD())
    {
        rtc->hours = (rtc->hours & 0x0F) + ((rtc->hours / 16) * 10);
        rtc->minutes = (rtc->minutes & 0x0F) + ((rtc->minutes / 16) * 10);
        rtc->seconds = (rtc->seconds & 0x0F) + ((rtc->seconds / 16) * 10);

        rtc->month_day = (rtc->month_day & 0x0F) + ((rtc->month_day / 16) * 10);
        rtc->month = (rtc->month & 0x0F) + ((rtc->month / 16) * 10);
        rtc->year = (rtc->year & 0x0F) + ((rtc->year / 16) * 10) + 2000;
    }
}
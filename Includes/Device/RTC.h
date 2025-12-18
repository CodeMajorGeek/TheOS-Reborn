#ifndef _RTC_H
#define _RTC_H

#include <stdbool.h>
#include <stdint.h>

#define RTC_SECONDS_REG             0x00
#define RTC_MINUTES_REG             0x02
#define RTC_HOURS_REG               0x04
#define RTC_WEEKDAY_REG             0x06
#define RTC_MONTH_DAY_REG           0x07
#define RTC_MONTH_REG               0x08
#define RTC_YEAR_REG                0x09
#define RTC_CENTURY_REG             0x32
#define RTC_STATUS_REG_A            0x0A
#define RTC_STATUS_REG_B            0x0B

#define RTC_UPDATE_IN_PROGRESS_FLAG (1 << 7)
#define RTC_USE_BCD_FLAG            (1 << 3)

#define RTC_WEEKDAY_LEN             4

typedef struct RTC
{
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
    char weekday[RTC_WEEKDAY_LEN];
    uint8_t month_day;
    uint8_t month;
    uint16_t year;

} RTC_t;

bool RTC_update_in_progress(void);
void RTC_wait_ready(void);

uint8_t RTC_read_status_b(void);

uint8_t RTC_read_seconds(void);
uint8_t RTC_read_minutes(void);
uint8_t RTC_read_hours(void);
uint8_t RTC_read_weekday(void);
uint8_t RTC_read_month_day(void);
uint8_t RTC_read_month(void);
uint8_t RTC_read_year(void);
uint8_t RTC_read_century(void);

void RTC_read(RTC_t* rtc_out);

#endif
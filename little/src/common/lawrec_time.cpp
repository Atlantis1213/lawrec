#include "lawrec_time.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" int lawrec_time_parse_utc(const char *text, int64_t *seconds)
{
    if (!text || !seconds || strnlen(text, 20) != 19) return -EINVAL;
    for (unsigned i = 0; i < 19; ++i) {
        char separator = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
        if (separator ? text[i] != separator : text[i] < '0' || text[i] > '9') return -EINVAL;
    }
    int year, month, day, hour, minute, second;
    if (sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        return -EINVAL;
    if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        hour > 23 || minute > 59 || second > 59) return -ERANGE;
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_day = days[month-1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day > max_day) return -ERANGE;
    struct tm value{};
    value.tm_year = year-1900; value.tm_mon = month-1; value.tm_mday = day;
    value.tm_hour = hour; value.tm_min = minute; value.tm_sec = second;
    time_t result = timegm(&value);
    if (result == time_t(-1)) return -ERANGE;
    *seconds = result;
    return 0;
}

extern "C" int lawrec_time_current_utc(char *text, size_t size)
{
    if (!text || size < 20) return -EINVAL;
    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now)) return -errno;
    struct tm value{};
    if (!gmtime_r(&now.tv_sec, &value)) return -ERANGE;
    return strftime(text, size, "%Y-%m-%d %H:%M:%S", &value) ? 0 : -ENOSPC;
}

extern "C" int lawrec_time_set_utc(const char *text)
{
    int64_t seconds;
    int ret = lawrec_time_parse_utc(text, &seconds);
    if (ret) return ret;
    struct timespec target{};
    target.tv_sec = seconds;
    if (clock_settime(CLOCK_REALTIME, &target)) ret = -errno;
    fprintf(stderr, "[time] manual UTC=%s result=%d rtc_updated=0\n", text, ret);
    return ret;
}

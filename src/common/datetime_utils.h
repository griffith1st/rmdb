#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "errors.h"

inline bool is_datetime_leap_year(int year) {
    return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

inline int datetime_days_in_month(int year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_datetime_leap_year(year)) {
        return 29;
    }
    return days[month];
}

inline int parse_datetime_part(const std::string &text, int start, int len) {
    int value = 0;
    for (int i = 0; i < len; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') {
            throw InternalError("Invalid DATETIME literal");
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

inline int64_t parse_datetime_to_int64(const std::string &text) {
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':') {
        throw InternalError("Invalid DATETIME literal");
    }

    int year = parse_datetime_part(text, 0, 4);
    int month = parse_datetime_part(text, 5, 2);
    int day = parse_datetime_part(text, 8, 2);
    int hour = parse_datetime_part(text, 11, 2);
    int minute = parse_datetime_part(text, 14, 2);
    int second = parse_datetime_part(text, 17, 2);

    if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > datetime_days_in_month(year, month) || hour > 23 || minute > 59 || second > 59) {
        throw InternalError("Invalid DATETIME literal");
    }

    return static_cast<int64_t>(year) * 10000000000LL +
           static_cast<int64_t>(month) * 100000000LL +
           static_cast<int64_t>(day) * 1000000LL +
           static_cast<int64_t>(hour) * 10000LL +
           static_cast<int64_t>(minute) * 100LL +
           second;
}

inline std::string datetime_to_string(int64_t value) {
    int second = static_cast<int>(value % 100);
    value /= 100;
    int minute = static_cast<int>(value % 100);
    value /= 100;
    int hour = static_cast<int>(value % 100);
    value /= 100;
    int day = static_cast<int>(value % 100);
    value /= 100;
    int month = static_cast<int>(value % 100);
    int year = static_cast<int>(value / 100);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  year, month, day, hour, minute, second);
    return std::string(buf);
}

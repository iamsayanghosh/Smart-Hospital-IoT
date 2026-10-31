#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

/**
 * @brief Parses a floating-point number strictly from a C-string.
 *
 * Validates:
 * - Skips leading whitespace.
 * - Rejects leading '-' if minVal >= 0.0f.
 * - Requires at least one numeric digit parsed.
 * - Allows trailing whitespace, but strictly rejects any non-whitespace trailing characters (e.g. "12abc").
 * - Rejects non-finite values (NaN, Inf).
 * - Enforces bounds [minVal, maxVal].
 *
 * @param str Null-terminated string to parse.
 * @param minVal Minimum acceptable value (inclusive).
 * @param maxVal Maximum acceptable value (inclusive).
 * @param outVal Reference receiving parsed value upon success.
 * @return true if valid and in range; false otherwise.
 */
inline bool parseFloatStrict(const char *str, float minVal, float maxVal, float &outVal)
{
    if (str == nullptr) {
        return false;
    }
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        ++str;
    }
    if (*str == '\0') {
        return false;
    }
    if (minVal >= 0.0f && *str == '-') {
        return false;
    }

    char *endptr = nullptr;
    const float val = strtof(str, &endptr);
    if (endptr == str) {
        return false;
    }
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        ++endptr;
    }
    if (*endptr != '\0') {
        return false;
    }
    if (!isfinite(val)) {
        return false;
    }
    if (val < minVal || val > maxVal) {
        return false;
    }

    outVal = val;
    return true;
}

/**
 * @brief Parses a long integer strictly from a C-string.
 *
 * Validates:
 * - Skips leading whitespace.
 * - Rejects leading '-' if minVal >= 0.
 * - Requires at least one numeric digit parsed.
 * - Allows trailing whitespace, but strictly rejects any non-whitespace trailing characters (e.g. "12abc").
 * - Enforces bounds [minVal, maxVal].
 *
 * @param str Null-terminated string to parse.
 * @param minVal Minimum acceptable value (inclusive).
 * @param maxVal Maximum acceptable value (inclusive).
 * @param outVal Reference receiving parsed value upon success.
 * @return true if valid and in range; false otherwise.
 */
inline bool parseLongStrict(const char *str, long minVal, long maxVal, long &outVal)
{
    if (str == nullptr) {
        return false;
    }
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        ++str;
    }
    if (*str == '\0') {
        return false;
    }
    if (minVal >= 0 && *str == '-') {
        return false;
    }

    char *endptr = nullptr;
    const long val = strtol(str, &endptr, 10);
    if (endptr == str) {
        return false;
    }
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        ++endptr;
    }
    if (*endptr != '\0') {
        return false;
    }
    if (val < minVal || val > maxVal) {
        return false;
    }

    outVal = val;
    return true;
}

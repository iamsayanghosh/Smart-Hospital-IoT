#include <Arduino.h>
#include <unity.h>
#include "parser_utils.h"
#include "medication.h"

void test_parse_float_strict_valid() {
    float val = 0.0f;
    TEST_ASSERT_TRUE(parseFloatStrict("36.5", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.5f, val);
}

void test_parse_float_strict_whitespace() {
    float val = 0.0f;
    TEST_ASSERT_TRUE(parseFloatStrict("  \t 37.2 \r\n", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.2f, val);
}

void test_parse_float_strict_boundary() {
    float val = 0.0f;
    TEST_ASSERT_TRUE(parseFloatStrict("34.0", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 34.0f, val);
    TEST_ASSERT_TRUE(parseFloatStrict("42.0", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, val);
}

void test_parse_float_strict_out_of_range() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("33.9", 34.0f, 42.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("42.1", 34.0f, 42.0f, val));
}

void test_parse_float_strict_invalid_trailing() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("12abc", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("36.5x", 34.0f, 42.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("36.5.2", 34.0f, 42.0f, val));
}

void test_parse_float_strict_non_finite() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("nan", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("NAN", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("inf", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("-inf", -100.0f, 100.0f, val));
}

void test_parse_float_strict_negative_rejected_when_min_non_negative() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("-5.0", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("-0.01", 0.0f, 100.0f, val));
}

void test_parse_float_strict_empty_or_null() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict(nullptr, 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("   ", 0.0f, 100.0f, val));
}

void test_parse_long_strict_valid() {
    long val = 0;
    TEST_ASSERT_TRUE(parseLongStrict("75", 40, 140, val));
    TEST_ASSERT_EQUAL_INT32(75, val);
}

void test_parse_long_strict_boundary() {
    long val = 0;
    TEST_ASSERT_TRUE(parseLongStrict("40", 40, 140, val));
    TEST_ASSERT_EQUAL_INT32(40, val);
    TEST_ASSERT_TRUE(parseLongStrict("140", 40, 140, val));
    TEST_ASSERT_EQUAL_INT32(140, val);
}

void test_parse_long_strict_out_of_range() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("39", 40, 140, val));
    TEST_ASSERT_FALSE(parseLongStrict("141", 40, 140, val));
}

void test_parse_long_strict_rejects_float_string() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("30.0", 0, 100, val));
    TEST_ASSERT_FALSE(parseLongStrict("75.5", 40, 140, val));
}

void test_parse_long_strict_invalid_negative() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("-5", 5, 60, val));
}

void test_parse_long_strict_invalid_trailing() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("60secs", 5, 60, val));
    TEST_ASSERT_FALSE(parseLongStrict("12abc", 0, 100, val));
}

void test_medication_clamping() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, clampMedicationDosage(-10.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, clampMedicationDosage(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, clampMedicationDosage(50.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, clampMedicationDosage(80.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 85.0f, clampMedicationDosage(85.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, clampMedicationDosage(100.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, clampMedicationDosage(150.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, clampMedicationDosage(NAN));
}

void test_medication_slew_initial_zero() {
    uint32_t lastUpdateMs = 0;
    float applied = 10.0f;
    float goal = 50.0f;
    float result = stepMedicationSlewRate(applied, goal, 1000U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, result);
    TEST_ASSERT_EQUAL_UINT32(1000U, lastUpdateMs);
}

void test_medication_slew_20ms_normal() {
    uint32_t lastUpdateMs = 1000U;
    float applied = 10.0f;
    float goal = 50.0f;
    // 20 ms elapsed: maxIncrease = 5.0 mg/hr/s * 0.020 s = 0.10 mg/hr
    float result = stepMedicationSlewRate(applied, goal, 1020U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.10f, result);
    TEST_ASSERT_EQUAL_UINT32(1020U, lastUpdateMs);
}

void test_medication_slew_60s_interruption() {
    uint32_t lastUpdateMs = 1000U;
    float applied = 10.0f;
    float goal = 50.0f;
    // Interruption of 60,000 ms: should re-anchor timestamp without catch-up spike
    float result = stepMedicationSlewRate(applied, goal, 61000U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, result);
    TEST_ASSERT_EQUAL_UINT32(61000U, lastUpdateMs);

    // Next 20 ms cycle resumes normal slew limiting
    result = stepMedicationSlewRate(result, goal, 61020U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.10f, result);
    TEST_ASSERT_EQUAL_UINT32(61020U, lastUpdateMs);
}

void test_medication_slew_millis_wraparound() {
    // uint32_t wraps at 4294967295. Set baseline 16 ms before wrap.
    uint32_t lastUpdateMs = 0xFFFFFFF0U;
    float applied = 10.0f;
    float goal = 50.0f;
    // now = 10 ms after wrap. Total deltaMs = 16 + 10 = 26 ms.
    // maxIncrease = 5.0 * 0.026 = 0.13 mg/hr
    float result = stepMedicationSlewRate(applied, goal, 10U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.13f, result);
    TEST_ASSERT_EQUAL_UINT32(10U, lastUpdateMs);
}

void test_medication_downward_immediate() {
    uint32_t lastUpdateMs = 1000U;
    float applied = 60.0f;
    float goal = 20.0f;
    // Downward adjustment is applied immediately without slew rate limiting
    float result = stepMedicationSlewRate(applied, goal, 1020U, lastUpdateMs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, result);
    TEST_ASSERT_EQUAL_UINT32(1020U, lastUpdateMs);
}

void test_medication_safety_levels() {
    TEST_ASSERT_EQUAL(MEDICATION_SAFETY_NORMAL, getMedicationSafetyLevel(0.0f));
    TEST_ASSERT_EQUAL(MEDICATION_SAFETY_NORMAL, getMedicationSafetyLevel(79.9f));
    TEST_ASSERT_EQUAL(MEDICATION_SAFETY_WARNING, getMedicationSafetyLevel(80.0f));
    TEST_ASSERT_EQUAL(MEDICATION_SAFETY_CRITICAL, getMedicationSafetyLevel(80.1f));
    TEST_ASSERT_EQUAL(MEDICATION_SAFETY_CRITICAL, getMedicationSafetyLevel(100.0f));
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_float_strict_valid);
    RUN_TEST(test_parse_float_strict_whitespace);
    RUN_TEST(test_parse_float_strict_boundary);
    RUN_TEST(test_parse_float_strict_out_of_range);
    RUN_TEST(test_parse_float_strict_invalid_trailing);
    RUN_TEST(test_parse_float_strict_non_finite);
    RUN_TEST(test_parse_float_strict_negative_rejected_when_min_non_negative);
    RUN_TEST(test_parse_float_strict_empty_or_null);
    RUN_TEST(test_parse_long_strict_valid);
    RUN_TEST(test_parse_long_strict_boundary);
    RUN_TEST(test_parse_long_strict_out_of_range);
    RUN_TEST(test_parse_long_strict_rejects_float_string);
    RUN_TEST(test_parse_long_strict_invalid_negative);
    RUN_TEST(test_parse_long_strict_invalid_trailing);
    RUN_TEST(test_medication_clamping);
    RUN_TEST(test_medication_slew_initial_zero);
    RUN_TEST(test_medication_slew_20ms_normal);
    RUN_TEST(test_medication_slew_60s_interruption);
    RUN_TEST(test_medication_slew_millis_wraparound);
    RUN_TEST(test_medication_downward_immediate);
    RUN_TEST(test_medication_safety_levels);
    UNITY_END();
}

void loop() {}

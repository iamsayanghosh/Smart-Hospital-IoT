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

void test_medication_slew_initial_timestamp_zero() {
    uint32_t lastUpdateMs = 0U;
    float result = calculateSlewStep(20.0f, 50.0f, lastUpdateMs, 1000U);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, result);
    TEST_ASSERT_EQUAL_UINT32(1000U, lastUpdateMs);
}

void test_medication_slew_normal_20ms_increment() {
    uint32_t lastUpdateMs = 1000U;
    float result = calculateSlewStep(20.0f, 50.0f, lastUpdateMs, 1020U);
    // 5.0 mg/hr/s * 0.020 s = +0.10 mg/hr
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.1f, result);
    TEST_ASSERT_EQUAL_UINT32(1020U, lastUpdateMs);
}

void test_medication_slew_interruption_60s() {
    uint32_t lastUpdateMs = 1020U;
    // Interruption of 73980 ms >= 60000 ms: refreshes baseline without sudden burst
    float pausedResult = calculateSlewStep(20.1f, 50.0f, lastUpdateMs, 75000U);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.1f, pausedResult);
    TEST_ASSERT_EQUAL_UINT32(75000U, lastUpdateMs);

    // Next 20 ms cycle resumes normal slewing without being stuck
    float resumedResult = calculateSlewStep(pausedResult, 50.0f, lastUpdateMs, 75020U);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.2f, resumedResult);
    TEST_ASSERT_EQUAL_UINT32(75020U, lastUpdateMs);
}

void test_medication_slew_near_uint32_max_rollover() {
    uint32_t lastUpdateMs = UINT32_MAX - 19U;
    uint32_t now = 1U; // Unsigned delta: 1 - (UINT32_MAX - 19) = 21 ms
    float result = calculateSlewStep(20.0f, 50.0f, lastUpdateMs, now);
    // 5.0 mg/hr/s * 0.021 s = +0.105 mg/hr
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 20.105f, result);
    TEST_ASSERT_EQUAL_UINT32(1U, lastUpdateMs);
}

void test_medication_downward_immediate() {
    uint32_t lastUpdateMs = 1000U;
    float result = calculateSlewStep(60.0f, 20.0f, lastUpdateMs, 1020U);
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
    RUN_TEST(test_medication_slew_initial_timestamp_zero);
    RUN_TEST(test_medication_slew_normal_20ms_increment);
    RUN_TEST(test_medication_slew_interruption_60s);
    RUN_TEST(test_medication_slew_near_uint32_max_rollover);
    RUN_TEST(test_medication_downward_immediate);
    RUN_TEST(test_medication_safety_levels);
    UNITY_END();
}

void loop() {}

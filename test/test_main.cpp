#include <Arduino.h>
#include <unity.h>
#include "parser_utils.h"
#include "state.h"
#include "medication.h"

void test_parse_float_strict_valid() {
    float val = 0.0f;
    TEST_ASSERT_TRUE(parseFloatStrict("36.5", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.5f, val);
}

void test_parse_float_strict_whitespace() {
    float val = 0.0f;
    TEST_ASSERT_TRUE(parseFloatStrict("  37.2 \r\n", 34.0f, 42.0f, val));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.2f, val);
}

void test_parse_float_strict_invalid_trailing() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("12abc", 0.0f, 100.0f, val));
    TEST_ASSERT_FALSE(parseFloatStrict("36.5x", 34.0f, 42.0f, val));
}

void test_parse_float_strict_negative_rejected_when_min_positive() {
    float val = 0.0f;
    TEST_ASSERT_FALSE(parseFloatStrict("-5.0", 0.0f, 100.0f, val));
}

void test_parse_long_strict_valid() {
    long val = 0;
    TEST_ASSERT_TRUE(parseLongStrict("75", 40, 140, val));
    TEST_ASSERT_EQUAL_INT32(75, val);
}

void test_parse_long_strict_invalid_negative() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("-5", 5, 60, val));
}

void test_parse_long_strict_invalid_trailing() {
    long val = 0;
    TEST_ASSERT_FALSE(parseLongStrict("60secs", 5, 60, val));
}

void test_medication_clamping() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, clampMedicationDosage(-10.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, clampMedicationDosage(50.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, clampMedicationDosage(80.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, clampMedicationDosage(150.0f));
}

void test_medical_alert_evaluation_nominal() {
    Telemetry normalTelemetry = {};
    normalTelemetry.heartRate = 75;
    normalTelemetry.spo2 = 98;
    normalTelemetry.ecgRate = 75;
    normalTelemetry.temperature = 36.8f;
    normalTelemetry.temperatureValid = true;

    AlertCandidate alert = evaluateMedicalAlert(normalTelemetry);
    TEST_ASSERT_FALSE(alert.active);
}

void test_medical_alert_evaluation_critical_hr() {
    Telemetry criticalTelemetry = {};
    criticalTelemetry.heartRate = 130; // > 120 critical
    criticalTelemetry.spo2 = 98;
    criticalTelemetry.ecgRate = 130;
    criticalTelemetry.temperature = 36.8f;
    criticalTelemetry.temperatureValid = true;

    AlertCandidate alert = evaluateMedicalAlert(criticalTelemetry);
    TEST_ASSERT_TRUE(alert.active);
    TEST_ASSERT_EQUAL_INT(ALERT_SEVERITY_CRITICAL, alert.severity);
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_parse_float_strict_valid);
    RUN_TEST(test_parse_float_strict_whitespace);
    RUN_TEST(test_parse_float_strict_invalid_trailing);
    RUN_TEST(test_parse_float_strict_negative_rejected_when_min_positive);
    RUN_TEST(test_parse_long_strict_valid);
    RUN_TEST(test_parse_long_strict_invalid_negative);
    RUN_TEST(test_parse_long_strict_invalid_trailing);
    RUN_TEST(test_medication_clamping);
    RUN_TEST(test_medical_alert_evaluation_nominal);
    RUN_TEST(test_medical_alert_evaluation_critical_hr);
    UNITY_END();
}

void loop() {}

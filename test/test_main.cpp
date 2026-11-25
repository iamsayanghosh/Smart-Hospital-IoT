#include <Arduino.h>
#include <unity.h>
#include "parser_utils.h"
#include "medication.h"
#include "bed.h"

// Define global medicalThresholds instance for unit testing environment
MedicalThresholdConfig medicalThresholds;

void setUp(void) {}
void tearDown(void) {}

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

void test_medical_telemetry_validity_distinguishes_normal_from_fault() {
    Telemetry snap = {};
    // 1. Initial boot: INITIALIZING
    TEST_ASSERT_TRUE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(snap));
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(snap));

    // 2. Confirmed temperature fault during boot before vitals/ECG arrive
    snap.initialTempProcessed = true;
    snap.temperatureValid = false;
    snap.physicalTempHealth = PHYSICAL_SENSOR_FAULT;
    snap.temperature = DEVICE_DISCONNECTED_C;
    TEST_ASSERT_TRUE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(snap));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(snap));

    // 3. Vitals received while temp still faulted
    snap.initialVitalsProcessed = true;
    TEST_ASSERT_TRUE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(snap));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(snap));

    // 4. Vitals and ECG received, temp still faulted
    snap.initialEcgProcessed = true;
    TEST_ASSERT_FALSE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(snap));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(snap));

    // 5. All valid: READY for clinical clearance
    snap.temperatureValid = true;
    snap.physicalTempHealth = PHYSICAL_SENSOR_HEALTHY;
    snap.temperature = 36.8f;
    TEST_ASSERT_FALSE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(snap));
    TEST_ASSERT_TRUE(isMedicalTelemetryFullyValid(snap));

    // 6. Runtime disconnect: revoked
    snap.temperatureValid = false;
    snap.physicalTempHealth = PHYSICAL_SENSOR_FAULT;
    snap.temperature = DEVICE_DISCONNECTED_C;
    TEST_ASSERT_FALSE(isMedicalTelemetryInitializing(snap));
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(snap));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(snap));
}

void test_medical_sensor_fault_state_machine_and_recovery() {
    Telemetry snap = {};
    bool sensorFaultActive = false;
    bool normalPending = false;

    // Unit test of state-transition bookkeeping functions (evaluateSensorFaultTransition,
    // applySensorFaultTransition); does not exercise MQTT network transport.

    // 1. Boot: INITIALIZING without temp attempt -> no transition
    SensorFaultTransition trans = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(snap),
        isMedicalTelemetryFullyValid(snap),
        sensorFaultActive
    );
    TEST_ASSERT_EQUAL(SensorFaultTransition::NONE, trans);
    TEST_ASSERT_FALSE(sensorFaultActive);

    // 2. Confirmed temp failure during boot triggers FAULT
    snap.initialTempProcessed = true;
    snap.temperatureValid = false;
    snap.physicalTempHealth = PHYSICAL_SENSOR_FAULT;
    snap.temperature = DEVICE_DISCONNECTED_C;
    trans = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(snap),
        isMedicalTelemetryFullyValid(snap),
        sensorFaultActive
    );
    TEST_ASSERT_EQUAL(SensorFaultTransition::TRIGGER_FAULT, trans);

    // Commit state transition (simulating safePublish / FIFO acceptance)
    TEST_ASSERT_TRUE(applySensorFaultTransition(trans, sensorFaultActive, normalPending));
    TEST_ASSERT_TRUE(sensorFaultActive);
    TEST_ASSERT_FALSE(normalPending);

    // Redundant evaluation while fault is active returns NONE
    trans = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(snap),
        isMedicalTelemetryFullyValid(snap),
        sensorFaultActive
    );
    TEST_ASSERT_EQUAL(SensorFaultTransition::NONE, trans);

    // 3. While faulted, acute vitals abnormalities preserved and detected by production evaluator
    snap.heartRate = 135;
    MedicalThresholdConfig testThresholds;
    AlertCandidate candidate = evaluateMedicalAlert(snap, testThresholds);
    TEST_ASSERT_TRUE(candidate.active);
    TEST_ASSERT_EQUAL_STRING("heartRate", candidate.parameter);
    TEST_ASSERT_EQUAL(ALERT_SEVERITY_CRITICAL, candidate.severity);

    // When acute abnormality resolves, normal blocked by fault
    snap.heartRate = 75;
    bool normalAllowed = !sensorFaultActive && isMedicalTelemetryFullyValid(snap);
    TEST_ASSERT_FALSE(normalAllowed);

    // 4. Recovery: Temperature valid and all channels ready triggers RECOVERY
    snap.initialVitalsProcessed = true;
    snap.initialEcgProcessed = true;
    snap.temperatureValid = true;
    snap.physicalTempHealth = PHYSICAL_SENSOR_HEALTHY;
    snap.temperature = 36.8f;
    TEST_ASSERT_TRUE(isMedicalTelemetryFullyValid(snap));

    trans = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(snap),
        isMedicalTelemetryFullyValid(snap),
        sensorFaultActive
    );
    TEST_ASSERT_EQUAL(SensorFaultTransition::TRIGGER_RECOVERY, trans);

    // Commit recovery transition
    TEST_ASSERT_TRUE(applySensorFaultTransition(trans, sensorFaultActive, normalPending));
    TEST_ASSERT_FALSE(sensorFaultActive);
    TEST_ASSERT_TRUE(normalPending);

    // 5. Clinical clearance allowed
    normalAllowed = !sensorFaultActive && isMedicalTelemetryFullyValid(snap);
    TEST_ASSERT_TRUE(normalAllowed);
}

void test_temperature_sensor_disconnect_queue_retry() {
    bool pendingTempDisconnectMessage = false;
    bool temperatureFaultReported = false;
    bool hasReadValidTemperature = false;
    uint8_t consecutiveScratchpadCount = 0;

    // Unit test of queue-retry, one-notification fault latch, physical acquisition history,
    // and DS18B20 scratchpad classification bookkeeping helpers
    // (updateQueueRetryState, updateTemperatureFaultReportState, retryTemperatureDisconnect,
    //  classifyDs18b20Scratchpad, recordPhysicalTemperatureAcquisition);
    // does not exercise FreeRTOS queue saturation or IPC primitives.

    // 1. Initial state: no valid physical conversion, no retry pending, no fault reported
    TEST_ASSERT_FALSE(hasReadValidTemperature);
    TEST_ASSERT_EQUAL_UINT8(0, consecutiveScratchpadCount);
    TEST_ASSERT_FALSE(pendingTempDisconnectMessage);
    TEST_ASSERT_FALSE(temperatureFaultReported);

    // 2. Cold boot 85.0 C scratchpad readings: attempts 1 and 2 (< 3) are ignored
    Ds18b20ScratchpadStatus status1 = classifyDs18b20Scratchpad(
        hasReadValidTemperature, pendingTempDisconnectMessage, consecutiveScratchpadCount);
    TEST_ASSERT_EQUAL(DS18B20_SCRATCHPAD_IGNORE_INITIAL, status1);
    TEST_ASSERT_EQUAL_UINT8(1, consecutiveScratchpadCount);

    Ds18b20ScratchpadStatus status2 = classifyDs18b20Scratchpad(
        hasReadValidTemperature, pendingTempDisconnectMessage, consecutiveScratchpadCount);
    TEST_ASSERT_EQUAL(DS18B20_SCRATCHPAD_IGNORE_INITIAL, status2);
    TEST_ASSERT_EQUAL_UINT8(2, consecutiveScratchpadCount);

    // 3. First valid physical DS18B20 conversion: marks physical history valid and resets counter
    recordPhysicalTemperatureAcquisition(hasReadValidTemperature, consecutiveScratchpadCount);
    TEST_ASSERT_TRUE(hasReadValidTemperature);
    TEST_ASSERT_EQUAL_UINT8(0, consecutiveScratchpadCount);

    // 4. Subsequent 85.0 C reading after valid physical conversion: immediately classified as runtime reset!
    Ds18b20ScratchpadStatus statusReset = classifyDs18b20Scratchpad(
        hasReadValidTemperature, pendingTempDisconnectMessage, consecutiveScratchpadCount);
    TEST_ASSERT_EQUAL(DS18B20_SCRATCHPAD_RUNTIME_RESET, statusReset);

    // 5. Failed queue delivery (sent = false) does NOT erase physical acquisition history
    bool queueSent = false;
    updateTemperatureFaultReportState(false, queueSent, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_TRUE(hasReadValidTemperature); // physical hardware history strictly preserved

    // 6. Queue full on initial disconnect dispatch (sendSuccess = false):
    //    Sets pending flag, does NOT latch faultReported
    bool changed = updateTemperatureFaultReportState(true, false, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_TRUE(pendingTempDisconnectMessage);
    TEST_ASSERT_FALSE(temperatureFaultReported);

    // 7. Queue full fault -> sensor recovery before queue space becomes available:
    //    Recovery attempt while pendingTempDisconnectMessage is true must NOT cancel pending fault
    bool recovered = updateTemperatureFaultReportState(false, true, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_FALSE(recovered);
    TEST_ASSERT_TRUE(pendingTempDisconnectMessage); // pending fault strictly preserved
    TEST_ASSERT_FALSE(temperatureFaultReported);

    // 8. Subsequent retry cycle with queue still full (sendSuccess = false) retains pending flag
    bool retried = retryTemperatureDisconnect(false, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_FALSE(retried);
    TEST_ASSERT_TRUE(pendingTempDisconnectMessage);
    TEST_ASSERT_FALSE(temperatureFaultReported);

    // 9. Queue drains: successful dispatch of pending disconnect (sendSuccess = true)
    //    Clears pending flag and latches faultReported
    retried = retryTemperatureDisconnect(true, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_TRUE(retried);
    TEST_ASSERT_FALSE(pendingTempDisconnectMessage);
    TEST_ASSERT_TRUE(temperatureFaultReported);

    // 10. Duplicate suppression: subsequent fault while faultReported is true does NOT dispatch duplicate
    bool duplicate = updateTemperatureFaultReportState(true, true, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_FALSE(duplicate);
    TEST_ASSERT_TRUE(temperatureFaultReported);

    // 11. Source-aware telemetry isolation via production helper applyTemperatureTelemetryUpdate:
    // a. Startup state: physical health is UNKNOWN; premature full clinical clearance is suppressed
    Telemetry testTelem = {};
    testTelem.initialVitalsProcessed = true;
    testTelem.initialEcgProcessed = true;
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_UNKNOWN, testTelem.physicalTempHealth);
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(testTelem));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(testTelem)); // suppressed: physical sensor not yet validated!

    // b. Valid simulated temperature arrives at startup before physical sensor completes conversion:
    //    measurement is available for OLED/monitoring, but physical health remains UNKNOWN
    applyTemperatureTelemetryUpdate(37.0f, true, testTelem);
    TEST_ASSERT_TRUE(testTelem.temperatureValid); // measurement available
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_UNKNOWN, testTelem.physicalTempHealth); // physical health unchanged!
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(testTelem));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(testTelem)); // premature NORMAL still suppressed!

    // c. Physical DS18B20 conversion succeeds: physical health confirmed HEALTHY, enabling full clinical clearance
    applyTemperatureTelemetryUpdate(36.8f, false, testTelem);
    TEST_ASSERT_TRUE(testTelem.temperatureValid);
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_HEALTHY, testTelem.physicalTempHealth);
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(testTelem));
    TEST_ASSERT_TRUE(isMedicalTelemetryFullyValid(testTelem)); // NORMAL now permitted!

    // d. Physical DS18B20 disconnects: sets physicalTempHealth to FAULT and triggers SENSOR FAULT
    applyTemperatureTelemetryUpdate(DEVICE_DISCONNECTED_C, false, testTelem);
    TEST_ASSERT_FALSE(testTelem.temperatureValid);
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_FAULT, testTelem.physicalTempHealth);
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(testTelem));
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(testTelem));

    bool faultActive = false;
    SensorFaultTransition transFault = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(testTelem),
        isMedicalTelemetryFullyValid(testTelem),
        faultActive);
    TEST_ASSERT_EQUAL(SensorFaultTransition::TRIGGER_FAULT, transFault);
    faultActive = true; // fault state committed

    // e. Simulated temperature override arrives during hardware fault: provides measurement availability
    //    without altering physicalTempHealth (remains FAULT) or triggering false SENSOR RESTORED
    applyTemperatureTelemetryUpdate(37.5f, true, testTelem);
    TEST_ASSERT_TRUE(testTelem.temperatureValid); // measurement available for clinical monitoring
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_FAULT, testTelem.physicalTempHealth); // physical hardware fault strictly preserved!
    TEST_ASSERT_TRUE(isMedicalTemperatureFault(testTelem)); // fault status remains active
    TEST_ASSERT_FALSE(isMedicalTelemetryFullyValid(testTelem)); // full clinical clearance suppressed!

    SensorFaultTransition transSim = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(testTelem),
        isMedicalTelemetryFullyValid(testTelem),
        faultActive);
    TEST_ASSERT_EQUAL(SensorFaultTransition::NONE, transSim); // false SENSOR RESTORED prevented!

    // f. Invalid simulated value does NOT classify as physical hardware fault
    Telemetry simInvalidTelem = {};
    simInvalidTelem.initialTempProcessed = true;
    simInvalidTelem.physicalTempHealth = PHYSICAL_SENSOR_UNKNOWN;
    applyTemperatureTelemetryUpdate(-127.0f, true, simInvalidTelem);
    TEST_ASSERT_FALSE(simInvalidTelem.temperatureValid);
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_UNKNOWN, simInvalidTelem.physicalTempHealth); // not blamed on hardware!
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(simInvalidTelem));

    // g. Simulated telemetry dispatch leaves physical task latches and scratchpad counter untouched
    TEST_ASSERT_TRUE(temperatureFaultReported); // physical task fault latch preserved
    TEST_ASSERT_TRUE(hasReadValidTemperature);  // physical acquisition history preserved

    // h. Physical sensor recovery: valid physical conversion sets HEALTHY and triggers SENSOR RESTORED
    applyTemperatureTelemetryUpdate(36.6f, false, testTelem);
    TEST_ASSERT_TRUE(testTelem.temperatureValid);
    TEST_ASSERT_EQUAL(PHYSICAL_SENSOR_HEALTHY, testTelem.physicalTempHealth);
    TEST_ASSERT_FALSE(isMedicalTemperatureFault(testTelem));
    TEST_ASSERT_TRUE(isMedicalTelemetryFullyValid(testTelem));

    SensorFaultTransition transRecover = evaluateSensorFaultTransition(
        isMedicalTemperatureFault(testTelem),
        isMedicalTelemetryFullyValid(testTelem),
        faultActive);
    TEST_ASSERT_EQUAL(SensorFaultTransition::TRIGGER_RECOVERY, transRecover);
    faultActive = false; // recovery committed

    // 12. Physical sensor recovery with space in queue (sendSuccess = true):
    //     Clears faultReported latch only upon physical valid conversion
    recovered = updateTemperatureFaultReportState(false, true, pendingTempDisconnectMessage, temperatureFaultReported);
    TEST_ASSERT_TRUE(recovered);
    TEST_ASSERT_FALSE(temperatureFaultReported);
    TEST_ASSERT_FALSE(pendingTempDisconnectMessage);

    // 13. Persistent scratchpad detection on cold boot (attempt 3+)
    bool freshValid = false;
    uint8_t freshCount = 2; // already 2 attempts
    Ds18b20ScratchpadStatus statusPersistent = classifyDs18b20Scratchpad(
        freshValid, false, freshCount);
    TEST_ASSERT_EQUAL(DS18B20_SCRATCHPAD_PERSISTENT_FAULT, statusPersistent);
    TEST_ASSERT_EQUAL_UINT8(3, freshCount);

    // 14. Legacy updateQueueRetryState helper verification
    bool legacyPending = false;
    TEST_ASSERT_TRUE(updateQueueRetryState(false, legacyPending));
    TEST_ASSERT_TRUE(legacyPending);
    TEST_ASSERT_TRUE(updateQueueRetryState(true, legacyPending));
    TEST_ASSERT_FALSE(legacyPending);
}

void test_evaluate_medical_alert_default_thresholds_override() {
    Telemetry snap = {};
    snap.spo2 = 98;
    snap.heartRate = 75;
    snap.ecgRate = 75;
    snap.temperature = 37.0f;
    snap.temperatureValid = true;

    // Normal baseline: no alert
    AlertCandidate cand = evaluateMedicalAlert(snap);
    TEST_ASSERT_FALSE(cand.active);

    // Modify global medicalThresholds temporarily
    float oldHrHigh = medicalThresholds.heartRate.high;
    medicalThresholds.heartRate.high = 70.0f; // Lower high threshold below current 75 BPM

    cand = evaluateMedicalAlert(snap); // Call without passing explicit threshold argument
    TEST_ASSERT_TRUE(cand.active);
    TEST_ASSERT_EQUAL_STRING("heartRate", cand.parameter);
    TEST_ASSERT_EQUAL(ALERT_SEVERITY_WARNING, cand.severity);

    // Restore threshold
    medicalThresholds.heartRate.high = oldHrHigh;
}

void test_bed_emergency_retention_on_sensor_disconnect() {
    Telemetry snap = {};
    snap.heartRate = 75;
    snap.spo2 = 98;
    snap.ecgRate = 75;
    snap.temperatureValid = true;
    snap.temperature = 39.8f; // Critical high fever (>39.5 C)

    bool tempTriggeredEmergency = false;

    // 1. Critical fever triggers EMERGENCY mode and sets latch
    BedMode mode = evaluateBedMode(snap, tempTriggeredEmergency);
    TEST_ASSERT_EQUAL(BED_MODE_EMERGENCY, mode);
    TEST_ASSERT_TRUE(tempTriggeredEmergency);

    // 2. Sensor disconnects (temperature becomes invalid)
    snap.temperatureValid = false;
    snap.temperature = DEVICE_DISCONNECTED_C;

    // 3. Posture MUST retain EMERGENCY (90 deg) and not drop to SLEEPING (10 deg)
    mode = evaluateBedMode(snap, tempTriggeredEmergency);
    TEST_ASSERT_EQUAL(BED_MODE_EMERGENCY, mode);
    TEST_ASSERT_TRUE(tempTriggeredEmergency);

    // 4. Valid temperature recovers at normal stable 36.8 C
    snap.temperatureValid = true;
    snap.temperature = 36.8f;

    // 5. Posture cleanly demotes to SLEEPING and clears latch
    mode = evaluateBedMode(snap, tempTriggeredEmergency);
    TEST_ASSERT_EQUAL(BED_MODE_SLEEPING, mode);
    TEST_ASSERT_FALSE(tempTriggeredEmergency);
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
    RUN_TEST(test_medical_telemetry_validity_distinguishes_normal_from_fault);
    RUN_TEST(test_medical_sensor_fault_state_machine_and_recovery);
    RUN_TEST(test_temperature_sensor_disconnect_queue_retry);
    RUN_TEST(test_evaluate_medical_alert_default_thresholds_override);
    RUN_TEST(test_bed_emergency_retention_on_sensor_disconnect);
    UNITY_END();
}

void loop() {}

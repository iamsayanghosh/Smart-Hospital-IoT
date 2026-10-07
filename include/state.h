#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

extern QueueHandle_t sensorQueue;
extern QueueHandle_t samplingQueue;
extern SemaphoreHandle_t displayMutex;
extern SemaphoreHandle_t alertSemaphore;
extern SemaphoreHandle_t heartbeatMutex;
extern portMUX_TYPE telemetryMux;
extern QueueHandle_t bedQueue;

// ============================================================================
// SHARED TELEMETRY STATE
// ============================================================================

enum SensorType {
    TEMP_DATA,
    MOTION_DATA,
    VITAL_DATA,
    ECG_DATA
};

enum MonitoredTaskId {
    TASK_TEMP,
    TASK_MOTION,
    TASK_VITAL,
    TASK_ECG,
    TASK_MQTT,
    TASK_DISPLAY,
    TASK_ALERT,
    TASK_MEDICATION,
    TASK_SAMPLING,
    TASK_BED,
    TASK_OFFLINE,
    TASK_MONITORED_COUNT
};

enum AlertSeverity {
    ALERT_SEVERITY_NORMAL = 0,
    ALERT_SEVERITY_WARNING = 1,
    ALERT_SEVERITY_CRITICAL = 2
};

enum MedicationSafetyLevel {
    MEDICATION_SAFETY_NORMAL = 0,
    MEDICATION_SAFETY_WARNING = 1,
    MEDICATION_SAFETY_CRITICAL = 2
};

struct TaskHeartbeat {
    uint32_t lastHeartbeatMs = 0;
    bool active = false;
};

struct ThresholdRange {
    float low = 0.0f;
    float high = 0.0f;
};

struct MedicalThresholdConfig {
    ThresholdRange heartRate = {50.0f, 110.0f};
    ThresholdRange spo2 = {92.0f, 100.0f};
    ThresholdRange bodyTemperature = {35.0f, 38.0f};
};

struct FacilityThresholdConfig {
    ThresholdRange roomTemperature = {18.0f, 28.0f};
    ThresholdRange oxygen = {19.5f, 24.0f};
    ThresholdRange aqi = {0.0f, 100.0f};
};

struct SensorMessage {
    SensorType type;
    float temperature;
    int motion;
    int heartRate;
    int spo2;
    int ecgRate;
    float ecgSample;
};

struct Telemetry {
    float temperature = 0.0;
    bool temperatureValid = false;
    int motion = 0;
    int heartRate = 75;
    int spo2 = 97;
    int ecgRate = 72;
    float ecgSample = 0.0f;
    bool alertActive = false;
    bool initialVitalsProcessed = false;
    bool initialEcgProcessed = false;
};

struct FacilityTelemetry {
    float roomTemperature = 22.0f;
    float oxygenLevel = 20.8f;
    int aqi = 35;
    bool alertActive = false;
};

struct MedicationDosageControl {
    float requested = 0.0f;
    float applied = 0.0f;
    bool received = false;
    uint32_t lastReceivedMs = 0;
    uint32_t lastRateLimitUpdateMs = 0;
    bool criticalConfirmationPending = false;
    bool criticalConfirmed = false;
    bool buttonPressed = false;
    uint32_t lastButtonTransitionMs = 0;
};

struct AlertCandidate {
    bool active = false;
    const char *parameter = nullptr;
    float value = 0.0f;
    const char *message = nullptr;
    AlertSeverity severity = ALERT_SEVERITY_NORMAL;
};

struct MedicalSummaryStats {
    uint32_t sampleCount = 0;
    float heartRateTotal = 0.0f;
    float bodyTemperatureTotal = 0.0f;
    int minSpo2 = 100;
    float maxBodyTemperature = 0.0f;
    uint32_t abnormalCount = 0;
};

struct FacilitySummaryStats {
    uint32_t sampleCount = 0;
    float roomTemperatureTotal = 0.0f;
    float oxygenTotal = 0.0f;
    float aqiTotal = 0.0f;
    uint32_t abnormalCount = 0;
};

struct SamplingPeriods {
    uint32_t temperatureMs = 2000;
    uint32_t vitalMs = 2000;
    uint32_t ecgMs = 200;
};

extern TaskHandle_t monitoredHandles[TASK_MONITORED_COUNT];
extern TaskHandle_t watchdogHandle;

extern Telemetry telemetry;
extern FacilityTelemetry facilityTelemetry;
extern MedicationDosageControl medicationDosageControl;
extern MedicalThresholdConfig medicalThresholds;
extern FacilityThresholdConfig facilityThresholds;
extern MedicalSummaryStats medicalSummaryStats;
extern FacilitySummaryStats facilitySummaryStats;
extern SamplingPeriods samplingPeriods;
extern bool medicalAlertPublished;
extern bool facilityAlertPublished;
extern String lastMedicalAlertParameter;
extern String lastFacilityAlertParameter;
extern AlertSeverity lastMedicalAlertSeverity;
extern AlertSeverity lastFacilityAlertSeverity;
extern uint32_t lastMedicalAlertPublishMs;
extern uint32_t lastFacilityAlertPublishMs;
extern bool medicalAlertStateKnown;
extern bool facilityAlertStateKnown;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

bool isAbnormal(const Telemetry &data);
bool isFacilityAbnormal(const FacilityTelemetry &data);
MedicationDosageControl getMedicationDosageSnapshot();
Telemetry getTelemetrySnapshot();
FacilityTelemetry getFacilitySnapshot();
bool sendSensorMessage(const SensorMessage &message, const char *taskName);
AlertCandidate evaluateMedicalAlert(const Telemetry &snapshot);
AlertCandidate evaluateFacilityAlert(const FacilityTelemetry &snapshot);
void updateMedicalSummaryStats(const Telemetry &snapshot);
void updateFacilitySummaryStats(const FacilityTelemetry &snapshot);

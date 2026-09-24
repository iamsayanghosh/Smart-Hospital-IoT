#include "state.h"

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

QueueHandle_t sensorQueue;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t alertSemaphore;
SemaphoreHandle_t heartbeatMutex;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// SHARED TELEMETRY STATE
// ============================================================================

TaskHandle_t monitoredHandles[TASK_MONITORED_COUNT] = {nullptr};
TaskHandle_t watchdogHandle = nullptr;
TaskHeartbeat taskHeartbeats[TASK_MONITORED_COUNT];
Telemetry telemetry;
FacilityTelemetry facilityTelemetry;
MedicationDosageControl medicationDosageControl;
MedicalThresholdConfig medicalThresholds;
FacilityThresholdConfig facilityThresholds;
MedicalSummaryStats medicalSummaryStats;
FacilitySummaryStats facilitySummaryStats;
SamplingPeriods samplingPeriods;

bool medicalAlertPublished = false;
bool facilityAlertPublished = false;
String lastMedicalAlertParameter = "";
String lastFacilityAlertParameter = "";
AlertSeverity lastMedicalAlertSeverity = ALERT_SEVERITY_NORMAL;
AlertSeverity lastFacilityAlertSeverity = ALERT_SEVERITY_NORMAL;
uint32_t lastMedicalAlertPublishMs = 0;
uint32_t lastFacilityAlertPublishMs = 0;
bool medicalAlertStateKnown = false;
bool facilityAlertStateKnown = false;

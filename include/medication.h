#ifndef MEDICATION_H
#define MEDICATION_H

#include <Arduino.h>
#include "state.h"

void medicationInit();
void medicationTask(void *parameter);
float clampMedicationDosage(float value);
MedicationSafetyLevel getMedicationSafetyLevel(float dosage);
const char *getMedicationSafetyLabel(float dosage);

#endif

import re
import sys

with open('src/main.cpp', 'r') as f:
    main_code = f.read()

# Functions that go to state.cpp
to_state = [
    'getTelemetrySnapshot',
    'getFacilitySnapshot',
    'sendSensorMessage',
    'evaluateMedicalAlert',
    'evaluateFacilityAlert',
    'simulateFacilityData',
    'updateMedicalSummaryStats',
    'updateFacilitySummaryStats',
    'resetSummaryStats'
]

to_delete = to_state + [
    'getAioFeedTopic',
    'buildAlertPayload',
    'buildMedicalSummaryPayload',
    'buildFacilitySummaryPayload',
    'buildHospitalSummaryPayload',
    'configureMqttBuffer',
    'handleMedicationDosagePayload',
    'mqttMessageCallback',
    'connectMqtt',
    'publishLiveFeed',
    'resetLiveFeedPublishState',
    'publishMedicalAlertIfNeeded',
    'publishFacilityAlertIfNeeded',
    'publishSummaryData',
    'mqttTask',
    'displayInit',
    'updateTelemetry',
    'displayTask',
    'isAbnormal',
    'isFacilityAbnormal',
    'clampMedicationDosage',
    'getMedicationDosageSnapshot',
    'getMedicationSafetyLevel',
    'getMedicationSafetyLabel',
    'updateMedicationDosageFromRequest'
]

def extract_function(name, code):
    pattern = r'(/\*\*.*?@brief.*?\n.*?\*/\n)?(.*?)\s+' + name + r'\s*\([^)]*\)\s*\{'
    match = re.search(pattern, code, re.DOTALL)
    if not match:
        pattern = r'(.*?)\s+' + name + r'\s*\([^)]*\)\s*\{'
        match = re.search(pattern, code, re.DOTALL)
        if not match:
            return None, code
        
    start_index = match.start()
    brace_count = 0
    in_string = False
    in_char = False
    escape = False
    
    body_start = code.find('{', start_index)
    if body_start == -1: return None, code
    
    for i in range(body_start, len(code)):
        c = code[i]
        if escape:
            escape = False
            continue
        if c == '\\':
            escape = True
            continue
        if c == '"' and not in_char:
            in_string = not in_string
            continue
        if c == "'" and not in_string:
            in_char = not in_char
            continue
            
        if not in_string and not in_char:
            if c == '{':
                brace_count += 1
            elif c == '}':
                brace_count -= 1
                if brace_count == 0:
                    end_index = i + 1
                    func_str = code[start_index:end_index]
                    
                    # Also eat trailing newlines
                    while end_index < len(code) and code[end_index] in '\r\n':
                        end_index += 1
                        
                    new_code = code[:start_index] + code[end_index:]
                    return func_str, new_code
    return None, code

extracted_for_state = []
for name in to_state:
    func_str, code_temp = extract_function(name, main_code)
    if func_str:
        extracted_for_state.append(func_str)
        print(f"Extracted {name}")
    else:
        print(f"Warning: Could not extract {name} for state")

with open('src/state.cpp', 'a') as f:
    f.write('\n\n')
    f.write('\n\n'.join(extracted_for_state))
    f.write('\n')
print(f"Appended {len(extracted_for_state)} functions to state.cpp")

for name in to_delete:
    count = 0
    while count < 3: # prevent infinite loop
        func_str, new_code = extract_function(name, main_code)
        if not func_str:
            break
        main_code = new_code
        print(f"Deleted {name} from main.cpp")
        count += 1

with open('src/main.cpp', 'w') as f:
    f.write(main_code)
print("Done.")

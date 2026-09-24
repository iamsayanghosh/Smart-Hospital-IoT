import sys

def process_file():
    with open('src/main.cpp', 'r') as f:
        lines = f.readlines()
        
    to_state = [
        'getTelemetrySnapshot', 'getFacilitySnapshot', 'sendSensorMessage', 
        'evaluateMedicalAlert', 'evaluateFacilityAlert', 'simulateFacilityData', 
        'updateMedicalSummaryStats', 'updateFacilitySummaryStats', 'resetSummaryStats'
    ]
    
    to_delete = to_state + [
        'getAioFeedTopic', 'buildAlertPayload', 'buildMedicalSummaryPayload', 
        'buildFacilitySummaryPayload', 'buildHospitalSummaryPayload', 
        'configureMqttBuffer', 'handleMedicationDosagePayload', 'mqttMessageCallback', 
        'connectMqtt', 'publishLiveFeed', 'resetLiveFeedPublishState', 
        'publishMedicalAlertIfNeeded', 'publishFacilityAlertIfNeeded', 
        'publishSummaryData', 'mqttTask', 'displayInit', 'updateTelemetry', 
        'displayTask', 'isAbnormal', 'isFacilityAbnormal', 'clampMedicationDosage', 
        'getMedicationDosageSnapshot', 'getMedicationSafetyLevel', 
        'getMedicationSafetyLabel', 'updateMedicationDosageFromRequest'
    ]

    extracted_for_state = []
    
    def find_and_remove(name, extract=False):
        nonlocal lines
        start_idx = -1
        for i, line in enumerate(lines):
            # very simple check
            if name in line and '(' in line and ')' in line and not line.strip().startswith('//'):
                # confirm it is a function definition
                if line.strip().endswith('{') or (i+1 < len(lines) and lines[i+1].strip().startswith('{')):
                    start_idx = i
                    break
        
        if start_idx == -1:
            return False
            
        # check if there's a comment block before it
        comment_start = start_idx
        while comment_start > 0 and (lines[comment_start-1].strip().startswith('*') or lines[comment_start-1].strip().startswith('/*')):
            comment_start -= 1
        if comment_start > 0 and lines[comment_start-1].strip() == '':
            comment_start -= 1
            
        # find end brace
        brace_count = 0
        end_idx = -1
        in_func = False
        
        for i in range(start_idx, len(lines)):
            line = lines[i]
            for c in line:
                if c == '{':
                    brace_count += 1
                    in_func = True
                elif c == '}':
                    brace_count -= 1
                    if in_func and brace_count == 0:
                        end_idx = i
                        break
            if in_func and brace_count == 0:
                break
                
        if end_idx != -1:
            func_lines = lines[comment_start:end_idx+1]
            if extract:
                extracted_for_state.append("".join(func_lines))
            del lines[comment_start:end_idx+1]
            return True
        return False

    for name in to_state:
        if find_and_remove(name, extract=True):
            print(f"Extracted {name}")
        else:
            print(f"Failed to find {name}")
            
    for name in to_delete:
        while find_and_remove(name, extract=False):
            print(f"Deleted extra {name}")
            
    with open('src/state.cpp', 'a') as f:
        f.write("\n\n" + "\n\n".join(extracted_for_state) + "\n")
        
    with open('src/main.cpp', 'w') as f:
        f.writelines(lines)

process_file()

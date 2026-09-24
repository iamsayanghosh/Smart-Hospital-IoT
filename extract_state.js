const fs = require('fs');
const content = fs.readFileSync('src/main.cpp', 'utf8');

function extractBlock(startRegex, endRegex) {
    const lines = content.split('\n');
    let startIdx = -1, endIdx = -1;
    for (let i = 0; i < lines.length; i++) {
        if (startIdx === -1 && startRegex.test(lines[i])) startIdx = i;
        if (startIdx !== -1 && endRegex.test(lines[i])) {
            endIdx = i;
            break;
        }
    }
    if (startIdx === -1 || endIdx === -1) throw new Error("Could not find block: " + startRegex);
    return {
        content: lines.slice(startIdx, endIdx + 1).join('\n'),
        startIdx,
        endIdx
    };
}

let isAbnormal = extractBlock(/bool isAbnormal\(const Telemetry &data\)/, /^}$/);
let isFacilityAbnormal = extractBlock(/bool isFacilityAbnormal\(const FacilityTelemetry &data\)/, /^}$/);
let clampMedication = extractBlock(/float clampMedicationDosage/, /^}$/);
let getMedSnapshot = extractBlock(/MedicationDosageControl getMedicationDosageSnapshot/, /^}$/);
let getMedLevel = extractBlock(/MedicationSafetyLevel getMedicationSafetyLevel/, /^}$/);
let getMedLabel = extractBlock(/const char \*getMedicationSafetyLabel/, /^}$/);
let updateMed = extractBlock(/void updateMedicationDosageFromRequest/, /^}$/);

// Wait, I need to make sure the regex matches the right closing brace.
// since we don't have nested braces in these functions, /^}$/ might work if there are no nested blocks ending at column 0.
// Actually, `updateMedicationDosageFromRequest` has nested blocks, but they are indented.
// To be safe, I'll extract by known line numbers!

// Let's just output the blocks and append them.

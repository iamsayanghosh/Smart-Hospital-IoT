const fs = require('fs');

const mainPath = 'src/main.cpp';
const mqttPath = 'src/mqtt.cpp';
const originalContent = fs.readFileSync(mainPath, 'utf8');

function extractBlock(startRegex, endRegex) {
    const lines = originalContent.split('\n');
    let startIdx = -1, endIdx = -1;
    for (let i = 0; i < lines.length; i++) {
        if (startIdx === -1 && startRegex.test(lines[i])) startIdx = i;
        if (startIdx !== -1 && endRegex.test(lines[i])) {
            endIdx = i;
            break;
        }
    }
    if (startIdx === -1 || endIdx === -1) throw new Error("Could not find block");
    return {
        content: lines.slice(startIdx, endIdx + 1).join('\n'),
        startIdx,
        endIdx
    };
}

const blocks = [];

blocks.push(extractBlock(/#define MEDICAL_HEART_RATE_FEED/, /#define MEDICATION_DOSAGE_FEED/));
blocks.push(extractBlock(/enum LiveFeedIndex/, /int lastPublishedAqi = 0;/));
blocks.push(extractBlock(/\/\*\*[\s\S]*?@brief Builds the Adafruit IO MQTT topic/, /String getAioFeedTopic.*?\n\{[\s\S]*?\n\}/)); // Wait, regex over lines array doesn't work for multi-line.

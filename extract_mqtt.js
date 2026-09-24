const fs = require('fs');
const content = fs.readFileSync('src/main.cpp', 'utf8');

const ext = (pattern) => {
    const match = content.match(pattern);
    if (!match) throw new Error("Match not found: " + pattern);
    return match[0];
};

const feeds = ext(/#define MEDICAL_HEART_RATE_FEED[\s\S]*?#define MEDICATION_DOSAGE_FEED "medication-dosage"/);
const liveFeeds = ext(/enum LiveFeedIndex[\s\S]*?int lastPublishedAqi = 0;/);
const getAioFeedTopic = ext(/\/\*\*[\s\S]*?String getAioFeedTopic.*?\{[\s\S]*?\n\}/);
const buildAlertPayload = ext(/\/\*\*[\s\S]*?String buildAlertPayload.*?\{[\s\S]*?\n\}/);
const buildMedicalSummary = ext(/\/\*\*[\s\S]*?String buildMedicalSummaryPayload.*?\{[\s\S]*?\n\}/);
const buildFacilitySummary = ext(/\/\*\*[\s\S]*?String buildFacilitySummaryPayload.*?\{[\s\S]*?\n\}/);
const buildHospitalSummary = ext(/\/\*\*[\s\S]*?String buildHospitalSummaryPayload.*?\{[\s\S]*?\n\}/);
const configMqttBuffer = ext(/\/\*\*[\s\S]*?void configureMqttBuffer.*?\{[\s\S]*?\n\}/);
const mqttSupport = ext(/\/\/\s*MQTT TASK SUPPORT[\s\S]*?void mqttTask.*?\{[\s\S]*?\n\}/);

let out = `#include "mqtt.h"
#include "app.h"
#include "state.h"
#include "connectivity.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include <PubSubClient.h>

`;
out += feeds + '\n\n';
out += liveFeeds + '\n\n';
out += getAioFeedTopic + '\n\n';
out += buildAlertPayload + '\n\n';
out += buildMedicalSummary + '\n\n';
out += buildFacilitySummary + '\n\n';
out += buildHospitalSummary + '\n\n';
out += configMqttBuffer + '\n\n';
out += mqttSupport + '\n';

fs.writeFileSync('src/mqtt.cpp', out);

// Now remove them from main.cpp
let newContent = content;
newContent = newContent.replace(feeds + '\n', '');
newContent = newContent.replace(liveFeeds + '\n', '');
newContent = newContent.replace(getAioFeedTopic + '\n', '');
newContent = newContent.replace(buildAlertPayload + '\n', '');
newContent = newContent.replace(buildMedicalSummary + '\n', '');
newContent = newContent.replace(buildFacilitySummary + '\n', '');
newContent = newContent.replace(buildHospitalSummary + '\n', '');
newContent = newContent.replace(configMqttBuffer + '\n', '');
newContent = newContent.replace(mqttSupport + '\n', '');

fs.writeFileSync('src/main.cpp', newContent);

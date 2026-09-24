const fs = require('fs');
const lines = fs.readFileSync('src/main.cpp', 'utf8').split('\n');

// Blocks to remove:
// 56-63 (feeds)
// 114-130 (liveFeedIndex)
// 172-178 (getAioFeedTopic)
// 180-196 (buildAlertPayload)
// 383-458 (buildSummary)
// 886-1293 (mqttTask)

const removeRanges = [
    [56, 63],
    [114, 130],
    [172, 178],
    [180, 196],
    [383, 458],
    [886, 1293]
];

function shouldRemove(index) {
    for (const [start, end] of removeRanges) {
        if (index >= start && index <= end) {
            return true;
        }
    }
    return false;
}

const newLines = lines.filter((_, index) => !shouldRemove(index));
fs.writeFileSync('src/main.cpp', newLines.join('\n'));

/**
 * @file offline_banner_gate.h
 * @brief Debounce for the "LOGGING OFFLINE" OLED banner (Task 6 UI).
 *
 * Pure logic (no Arduino/FreeRTOS dependency) so it can be unit-tested natively.
 *
 * The banner is shown only when BOTH hold:
 *  - the link has been continuously offline for at least holdMs, and
 *  - the device has been running for at least bootGraceMs (Wi-Fi/MQTT are
 *    still connecting right after power-up, which is not a real outage).
 * It is hidden immediately when the link recovers.
 */
#ifndef OFFLINE_BANNER_GATE_H
#define OFFLINE_BANNER_GATE_H

#include <stdint.h>

struct OfflineBannerGate {
    uint32_t holdMs;
    uint32_t bootGraceMs;
    uint32_t offlineSinceMs;
    bool tracking;
    bool graceDone;

    OfflineBannerGate(uint32_t hold, uint32_t grace)
        : holdMs(hold), bootGraceMs(grace), offlineSinceMs(0), tracking(false), graceDone(false) {}

    /** @return true when the banner should be drawn on this frame. */
    bool update(bool offline, uint32_t nowMs)
    {
        if (!graceDone && nowMs >= bootGraceMs) {
            graceDone = true; // latch so a millis() wrap (~49 days) cannot re-open the grace window
        }
        if (!offline) {
            tracking = false;
            return false;
        }
        if (!tracking) {
            tracking = true;
            offlineSinceMs = nowMs;
        }
        return graceDone && (uint32_t)(nowMs - offlineSinceMs) >= holdMs;
    }
};

#endif // OFFLINE_BANNER_GATE_H


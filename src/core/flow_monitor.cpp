#include "flow_monitor.h"

namespace tinkle {

FlowMonitor::FlowMonitor(float pulsesPerGallon)
    : k_(pulsesPerGallon > 0.0f ? pulsesPerGallon : 1.0f) {}

void FlowMonitor::setK(float pulsesPerGallon) {
    if (pulsesPerGallon > 0.0f) k_ = pulsesPerGallon;   // reject <= 0; never divide by zero
}

void FlowMonitor::resetAccumulation(uint32_t pulses, uint32_t nowMs) {
    baseline_     = pulses;
    latest_       = pulses;
    sampleCount_  = 0;
    sampleHead_   = 0;
    lastSampleMs_ = nowMs;
    pushSample(pulses, nowMs);          // seed the window with the starting point
}

void FlowMonitor::pushSample(uint32_t pulses, uint32_t nowMs) {
    sampleMs_[sampleHead_]     = nowMs;
    samplePulses_[sampleHead_] = pulses;
    sampleHead_ = static_cast<uint8_t>((sampleHead_ + 1) % RATE_SAMPLES);
    if (sampleCount_ < RATE_SAMPLES) ++sampleCount_;
    lastSampleMs_ = nowMs;
}

void FlowMonitor::tick(uint32_t pulses, uint32_t nowMs) {
    latest_ = pulses;
    // Unsigned subtraction is rollover-safe; one sample per RATE_SAMPLE_MS keeps the ring small.
    if (sampleCount_ == 0 || (uint32_t)(nowMs - lastSampleMs_) >= RATE_SAMPLE_MS)
        pushSample(pulses, nowMs);
}

float FlowMonitor::gallons() const {
    return static_cast<float>(latest_ - baseline_) / k_;
}

float FlowMonitor::rateGPM() const {
    if (sampleCount_ < 2) return 0.0f;
    // Oldest valid entry .. newest entry (head-1), walking the ring.
    const uint8_t newest = static_cast<uint8_t>((sampleHead_ + RATE_SAMPLES - 1) % RATE_SAMPLES);
    const uint8_t oldest = (sampleCount_ < RATE_SAMPLES)
                         ? 0
                         : sampleHead_;                 // full ring: head is the oldest
    const uint32_t dtMs = sampleMs_[newest] - sampleMs_[oldest];
    if (dtMs == 0) return 0.0f;
    const uint32_t dPulses = samplePulses_[newest] - samplePulses_[oldest];
    const float gallons = static_cast<float>(dPulses) / k_;
    const float minutes = static_cast<float>(dtMs) / 60000.0f;
    return gallons / minutes;
}

} // namespace tinkle

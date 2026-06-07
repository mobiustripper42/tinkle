#pragma once
#include <stdint.h>

// FlowMonitor — turns hall-sensor pulses into gallons + a live GPM rate (firmware
// spec §7, #34).
//
// Platform-independent (src/core): like the run clock, it consumes a MONOTONIC pulse
// count passed into tick() rather than owning the hardware — the ESP32 ISR + counter
// live in src/esp32/flow_sensor.h, and host tests inject pulse counts directly with a
// fake clock. No interrupts, no GPIO here.
//
// MODEL: pulses are a free-running cumulative count (the ISR only ever increments).
//   gallons since reset = (pulses - baseline) / K
//   rateGPM             = Δpulses / K over a rolling time window
// K is `pulsesPerGallon`, seeded from NVS (Persistence) and overwritten by calibration
// (#36). resetAccumulation() re-baselines at the start of each run (and each calibration
// run) so gallons is per-run; the rolling window decays to ~0 GPM when flow stops, which
// is what the §7 no-flow / unexpected-flow detection (#35) keys on.

namespace tinkle {

class FlowMonitor {
public:
    // Rolling-rate window: sample the cumulative count at ~1 Hz into a small ring; rateGPM
    // spans oldest→newest sample, so the window is up to (RATE_SAMPLES-1) seconds.
    static constexpr uint8_t  RATE_SAMPLES   = 8;
    static constexpr uint32_t RATE_SAMPLE_MS = 1000;

    explicit FlowMonitor(float pulsesPerGallon);

    // Calibration K. A non-positive value is rejected (kept at the prior K) — gallons math
    // must never divide by zero.
    void  setK(float pulsesPerGallon);
    float k() const { return k_; }

    // Baseline the counter + clear the rate window. Call at boot and whenever a fresh tally
    // should start (run start, calibration start).
    void begin(uint32_t pulses, uint32_t nowMs) { resetAccumulation(pulses, nowMs); }
    void resetAccumulation(uint32_t pulses, uint32_t nowMs);

    // Sample the current cumulative pulse count. Call every loop tick; the ring only takes a
    // new sample every RATE_SAMPLE_MS, but gallons() tracks the latest count each call.
    void tick(uint32_t pulses, uint32_t nowMs);

    float    gallons() const;                       // (latest - baseline) / K
    float    rateGPM() const;                       // 0 until two samples span the window
    uint32_t pulsesSinceReset() const { return latest_ - baseline_; }

private:
    void pushSample(uint32_t pulses, uint32_t nowMs);

    float    k_;
    uint32_t baseline_ = 0;     // pulse count at the last reset
    uint32_t latest_   = 0;     // most recent pulse count seen by tick()

    // Fixed ring of 1 Hz samples for the rolling rate (no dynamic allocation).
    uint32_t sampleMs_[RATE_SAMPLES]     = {0};
    uint32_t samplePulses_[RATE_SAMPLES] = {0};
    uint8_t  sampleCount_ = 0;   // valid entries, capped at RATE_SAMPLES
    uint8_t  sampleHead_  = 0;   // index of the next write (newest = head-1)
    uint32_t lastSampleMs_ = 0;
};

} // namespace tinkle

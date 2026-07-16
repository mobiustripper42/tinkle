#pragma once
#include <stdint.h>

// WifiLinkMonitor — recovery policy for an already-joined STA link (#147). Pure +
// host-tested; the ESP32 WifiManager feeds it the live connected flag each tick and performs
// the WiFi.* action it returns.
//
// The bug it fixes: WifiManager::tick() stopped watching the link after the initial join,
// leaning entirely on the SDK's setAutoReconnect — which wedges after an AP hiccup / DHCP
// churn / RSSI dip, leaving the box unreachable with no application-level recovery (the field
// "loads home then disconnects, never comes back" mode).
//
// Policy: after the link has been continuously down for `graceMs` (so a momentary blip doesn't
// thrash), emit Reconnect, then again every `retryIntervalMs` — INDEFINITELY. It deliberately
// never falls back to SoftAP on a mid-season drop: the farm AP returns and the box re-joins on
// its own, where a SoftAP fallback would strand it off-network until a reboot. (The initial-
// join SoftAP fallback, for first setup / bad creds, stays in WifiManager and is unaffected.)

namespace tinkle {

enum class WifiAction : uint8_t { None, Reconnect };

class WifiLinkMonitor {
public:
    struct Config {
        uint32_t graceMs         = 8000;    // continuous down-time before the first reconnect
        uint32_t retryIntervalMs = 15000;   // between reconnect attempts while still down
    };

    WifiLinkMonitor() {}
    explicit WifiLinkMonitor(const Config& cfg) : cfg_(cfg) {}

    // Feed the live link state each tick; returns the action to take. Reconnect is emitted at
    // most once per retryIntervalMs. A `connected` tick re-arms from a healthy baseline, so the
    // next drop starts its grace + retry cadence fresh.
    WifiAction update(bool connected, uint32_t nowMs) {
        if (connected) { connected_ = true; return WifiAction::None; }

        if (connected_) {                              // edge: just dropped
            connected_   = false;
            downSinceMs_ = nowMs;
            lastTryMs_   = nowMs - cfg_.retryIntervalMs;   // first retry eligible right after grace
        }
        if ((uint32_t)(nowMs - downSinceMs_) < cfg_.graceMs)          return WifiAction::None;
        if ((uint32_t)(nowMs - lastTryMs_)   < cfg_.retryIntervalMs)  return WifiAction::None;
        lastTryMs_ = nowMs;
        return WifiAction::Reconnect;
    }

    bool connected() const { return connected_; }

private:
    Config   cfg_;
    bool     connected_   = true;   // armed = connected (WifiManager only ticks this in Sta mode)
    uint32_t downSinceMs_ = 0;
    uint32_t lastTryMs_   = 0;
};

} // namespace tinkle

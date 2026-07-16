#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "../core/persistence.h"
#include "../core/wifi_link_monitor.h"   // #147 STA-drop recovery decision (host-tested)

// WifiManager — STA-join with stored creds, SoftAP fallback, mDNS (§10 / #55).
// Header-only ESP32 shim like the other bindings; the core never sees WiFi.
//
// Non-blocking: begin() only KICKS OFF the join; tick() watches it from the loop.
// No creds, or no association within STA_TIMEOUT_MS -> SoftAP "Tinkle-Setup" (open;
// the field tradeoff — a phone must reach the config page with wet hands, and the
// AP only exists when the farm mesh doesn't). mDNS claims tinkle.local in either
// mode. Credentials written via /api/settings apply at the NEXT boot — a mid-
// season re-join dance isn't worth the state machine (documented in §10 note).
//
// LOCAL AUTONOMY (§17): nothing here gates watering. The scheduler runs off NVS +
// the local clock; WiFi down forever just means no phone UI.

namespace tinkle {

class WifiManager {
public:
    enum class Mode : uint8_t { Connecting, Sta, SoftAp };

    static constexpr uint32_t STA_TIMEOUT_MS = 20000;

    explicit WifiManager(Persistence& store) : store_(store) {}

    void begin(uint32_t nowMs) {
        startMs_ = nowMs;
        WiFi.persistent(false);                 // NVS creds are ours (DEC-008), not the SDK's
        const char* ssid = store_.wifiSsid();
        const char* pass = store_.wifiPass();
#ifdef TINKLE_SIM
        // Wokwi (#62): a fresh sim has empty NVS, and the SoftAP fallback isn't
        // reachable from outside the simulator — default to Wokwi's open guest AP
        // so the net.forward'd SPA + NTP work out of the box. Stored creds (set
        // via /api/settings inside the sim) still win.
        if (ssid[0] == '\0') { ssid = "Wokwi-GUEST"; pass = ""; }
#endif
        if (ssid[0] != '\0') {
            WiFi.mode(WIFI_STA);
            WiFi.setAutoReconnect(true);
            WiFi.begin(ssid, pass);
            mode_ = Mode::Connecting;
        } else {
            startSoftAp();
        }
    }

    void tick(uint32_t nowMs) {
        if (mode_ == Mode::Connecting) {
            if (WiFi.status() == WL_CONNECTED) {
                mode_ = Mode::Sta;
                link_.update(true, nowMs);   // arm the drop monitor from a healthy baseline
                startMdns();
                Serial.printf("[tinkle] WiFi STA up: %s -> http://tinkle.local (%s)\n",
                              store_.wifiSsid(), WiFi.localIP().toString().c_str());
            } else if ((uint32_t)(nowMs - startMs_) >= STA_TIMEOUT_MS) {
                Serial.println(F("[tinkle] WiFi STA join timed out -> SoftAP fallback"));
                WiFi.disconnect(true);
                startSoftAp();
            }
            return;
        }

        // #147: once joined, keep WATCHING the STA link — the SDK's auto-reconnect wedges,
        // and the old code did nothing here, so a dropped link never recovered. On a sustained
        // drop, re-kick the join (indefinitely — no SoftAP trap); on re-association, re-announce
        // mDNS so tinkle.local isn't stale.
        if (mode_ == Mode::Sta) {
            const bool up = (WiFi.status() == WL_CONNECTED);
            if (up && !wasUp_) {
                startMdns();                 // re-announce after a reconnect
                Serial.printf("[tinkle] WiFi STA re-associated (%s)\n",
                              WiFi.localIP().toString().c_str());
            }
            wasUp_ = up;
            if (link_.update(up, nowMs) == WifiAction::Reconnect) {
                Serial.println(F("[tinkle] WiFi STA link down -> reconnecting"));
                WiFi.reconnect();
            }
        }
    }

    Mode mode() const { return mode_; }
    const char* modeName() const {
        switch (mode_) {
            case Mode::Connecting: return "connecting";
            case Mode::Sta:        return "sta";
            case Mode::SoftAp:     return "softAp";
        }
        return "?";
    }
    String ip() const {
        return mode_ == Mode::SoftAp ? WiFi.softAPIP().toString()
                                     : WiFi.localIP().toString();
    }
    int rssi() const { return mode_ == Mode::Sta ? WiFi.RSSI() : 0; }

private:
    void startSoftAp() {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Tinkle-Setup");            // open AP, exists only sans farm mesh
        mode_ = Mode::SoftAp;
        startMdns();
        Serial.printf("[tinkle] SoftAP up: Tinkle-Setup (%s)\n",
                      WiFi.softAPIP().toString().c_str());
    }
    void startMdns() {
        if (!mdnsUp_) mdnsUp_ = MDNS.begin("tinkle");
        if (mdnsUp_)  MDNS.addService("http", "tcp", 80);
    }

    Persistence&    store_;
    Mode            mode_    = Mode::Connecting;
    uint32_t        startMs_ = 0;
    bool            mdnsUp_  = false;
    bool            wasUp_   = true;   // #147: prev STA link state, for the re-associate edge
    WifiLinkMonitor link_;             // #147: sustained-drop -> reconnect decision
};

} // namespace tinkle

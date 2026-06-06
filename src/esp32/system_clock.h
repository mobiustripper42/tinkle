#pragma once
#include <Arduino.h>
#include <time.h>
#include "../core/clock.h"

// ESP32 binding for IWallClock — the only place Clock touches SNTP, the system RTC, and
// the timezone. Header-only, mirroring arduino_gpio.h / preferences_store.h (DEC-009).
//
// Timezone + DST live HERE, not in core: configTzTime installs a POSIX TZ rule, and every
// localtime_r applies it (including the DST shift). localEpoch() reads UTC via time(),
// then returns LOCAL epoch seconds = UTC + tm_gmtoff — so the core does pure modular math
// (epoch % 86400 → local time-of-day) with no timezone database of its own.
//
// NTP: configTzTime also sets the SNTP servers. Spec §13 calls it "on Wi-Fi join"; until
// WiFi lands (Phase 4) SNTP simply never gets a reply and time() stays at boot-epoch
// (1970) — localEpoch() reports NOT-synced, the display holds "--:--". The moment the
// network appears and the first NTP packet arrives, the next poll locks the clock valid.

namespace tinkle {

struct SystemClock : IWallClock {
    // Farm timezone — US Eastern with DST (2nd Sun Mar → 1st Sun Nov). POSIX TZ form.
    static constexpr const char* TZ_POSIX = "EST5EDT,M3.2.0/2,M11.1.0/2";
    static constexpr const char* NTP1     = "pool.ntp.org";
    static constexpr const char* NTP2     = "time.nist.gov";

    // The clock is "synced" once time() reads past a sane epoch (2024-01-01 UTC). Boot-
    // epoch (1970) reads below it, so an un-NTP'd system reports NOT-synced.
    static constexpr uint32_t SYNC_THRESHOLD = 1704067200u;

    // Configure TZ + SNTP. Harmless without a network (no replies arrive); Phase 4 re-
    // invokes / relocates this onto the WiFi-join event per §13.
    void begin() { configTzTime(TZ_POSIX, NTP1, NTP2); }

    bool localEpoch(uint32_t& epochOut) override {
        const time_t utc = time(nullptr);
        if (static_cast<uint32_t>(utc) < SYNC_THRESHOLD) return false;   // not yet NTP-synced
        struct tm lt;
        localtime_r(&utc, &lt);                                          // applies TZ + DST
        // Re-pack the local broken-down fields into a local epoch (ESP32 newlib omits
        // tm_gmtoff, so we can't just add an offset). The day boundary then sits at local
        // midnight and the core's weekday / time-of-day math lands on local values.
        epochOut = epochFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                                  lt.tm_hour, lt.tm_min, lt.tm_sec);
        return true;
    }
};

} // namespace tinkle

#include "clock.h"

namespace tinkle {

uint32_t epochFromCivil(int y, unsigned m, unsigned d,
                        unsigned hh, unsigned mm, unsigned ss) {
    // Howard Hinnant's days_from_civil: civil date -> days since 1970-01-01, exact and
    // branch-light. March-based year so the leap day lands at the end of the cycle.
    y -= (m <= 2);
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;          // [0, 146096]
    const long     days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
    return static_cast<uint32_t>(days * 86400 + hh * 3600 + mm * 60 + ss);
}

Clock::Clock(IWallClock& src) : src_(src) {}

void Clock::resync(uint32_t nowMs) {
    lastAttemptMs_ = nowMs;
    uint32_t e;
    if (src_.localEpoch(e)) {
        epochAnchor_ = e;
        msAnchor_    = nowMs;
        synced_      = true;
    }
}

void Clock::begin(uint32_t nowMs) {
    resync(nowMs);   // immediate first attempt; stays invalid until a source is ready
}

void Clock::tick(uint32_t nowMs) {
    // Brisk retry until locked on, then a slow hourly re-anchor so free-run is exercised.
    // The subtraction is unsigned — clean across the millis() rollover.
    const uint32_t interval = synced_ ? RESYNC_INTERVAL_MS : ATTEMPT_INTERVAL_MS;
    if (nowMs - lastAttemptMs_ >= interval) resync(nowMs);
}

uint32_t Clock::epoch(uint32_t nowMs) const {
    if (!synced_) return 0;
    // Free-run from the anchor. Unsigned elapsed wraps cleanly; resync keeps it bounded.
    return epochAnchor_ + (nowMs - msAnchor_) / 1000u;
}

WallTime Clock::wall(uint32_t nowMs) const {
    WallTime w = {0, 0, 0, 0};
    if (!synced_) return w;            // caller gates on valid(); "--:--" otherwise
    const uint32_t e = epoch(nowMs);
    w.second  = static_cast<uint8_t>(e % 60u);
    w.minute  = static_cast<uint8_t>((e / 60u) % 60u);
    w.hour    = static_cast<uint8_t>((e / 3600u) % 24u);
    // 1970-01-01 (local epoch day 0) is a Thursday; +4 maps it onto Sun=0..Sat=6.
    w.weekday = static_cast<uint8_t>(((e / 86400u) + 4u) % 7u);
    return w;
}

bool Clock::minuteRolled(uint32_t nowMs) {
    if (!synced_) return false;
    const uint32_t m = epoch(nowMs) / 60u;   // total local minutes since epoch
    if (!minuteSeen_ || m != lastMinute_) {
        minuteSeen_ = true;
        lastMinute_ = m;
        return true;
    }
    return false;
}

} // namespace tinkle

#include "telemetry_payload.h"
#include <stdio.h>

namespace tinkle {

bool isPublishable(const RunEntry& e) {
    // A real run with a trustworthy timestamp. See the header on why clockWasValid gates.
    return e.result != RunResult::None && e.clockWasValid;
}

size_t buildIrrigationTopic(const RunEntry& e, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    int n = snprintf(out, cap, "farm/irrigation/tinkle/zone%u",
                     (unsigned)(e.zoneIndex + 1));
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

const char* faultCodeToString(uint8_t faultCode) {
    // Mirrors RunController's Fault enum (run_controller.h). Kept as a local table so the
    // payload module doesn't pull the whole state machine in; update if Fault grows.
    switch (faultCode) {
        case 0:  return nullptr;            // None
        case 1:  return "no-flow";          // NoFlow — the far-end under-delivery case
        case 2:  return "unexpected-flow";  // UnexpectedFlow
        case 3:  return "watchdog";         // Watchdog
        case 4:  return "cal-range";        // CalRange
        case 5:  return "clock";            // Clock
        case 6:  return "valve-rest";       // ValveRest
        case 7:  return "missed-cycle";     // MissedCycle — a distributed cycle that never ran (#161)
        default: return "fault";            // unknown/added code — still surfaces a fault
    }
}

size_t buildIrrigationPayload(const RunEntry& e, const char* tsIso, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!isPublishable(e)) return 0;
    if (!tsIso || tsIso[0] == '\0') return 0;

    // centigallons (hundredths of a gallon, u16) -> integer.fraction with 2 decimals.
    const unsigned whole = (unsigned)e.centigallons / 100u;
    const unsigned frac  = (unsigned)e.centigallons % 100u;
    const char* const fert = e.fertigate ? "true" : "false";
    const char* const fault = faultCodeToString(e.faultCode);

    // Fixed-shape v1 object (field order is cosmetic; the store doesn't care). "fault" is
    // optional — emitted only on a faulted run. "trigger" is omitted: RunEntry doesn't
    // carry it and the contract makes it optional.
    int n;
    if (fault) {
        n = snprintf(out, cap,
            "{\"v\":%u,\"source\":\"tinkle\",\"zone\":%u,\"ts_start\":\"%s\","
            "\"duration_s\":%u,\"gallons\":%u.%02u,\"fertigated\":%s,\"fault\":\"%s\"}",
            (unsigned)POOPDECK_SCHEMA_V, (unsigned)(e.zoneIndex + 1), tsIso,
            (unsigned)e.durationSec, whole, frac, fert, fault);
    } else {
        n = snprintf(out, cap,
            "{\"v\":%u,\"source\":\"tinkle\",\"zone\":%u,\"ts_start\":\"%s\","
            "\"duration_s\":%u,\"gallons\":%u.%02u,\"fertigated\":%s}",
            (unsigned)POOPDECK_SCHEMA_V, (unsigned)(e.zoneIndex + 1), tsIso,
            (unsigned)e.durationSec, whole, frac, fert);
    }
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

} // namespace tinkle

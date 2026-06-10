#include "persistence.h"
#include <stdio.h>   // snprintf for zone-key formatting

namespace tinkle {

// Static keys. All <= 15 chars (NVS hard limit — Preferences silently TRUNCATES longer
// keys, which would alias siblings). The per-zone key is generated in zoneKey().
static constexpr const char* KEY_SCHEMA  = "schema_ver";  // 10
static constexpr const char* KEY_SW_MAX  = "sw_max_sec";  // 10
static constexpr const char* KEY_K_PPG   = "k_ppg";       //  5  flow K, pulses/gallon (§7)

const char* Persistence::zoneKey(char* buf, uint8_t zone) {
    // "z<N>_dur": longest is two-digit zone "z15_dur" = 7 chars + NUL. buf must be >= 8.
    int n = snprintf(buf, 8, "z%u_dur", static_cast<unsigned>(zone));
    // Guard the 15-char NVS limit at the source rather than discover a truncation
    // collision in the field (DEC-008). n<0 means encoding error; n>=8 means truncation.
    if (n < 0 || n >= 8) { buf[0] = 'z'; buf[1] = '?'; buf[2] = '\0'; }
    return buf;
}

Persistence::Persistence(IKeyValueStore& store, uint8_t zoneCount)
    : store_(store),
      zoneCount_(zoneCount > ValveConfig::MAX_ZONES ? ValveConfig::MAX_ZONES : zoneCount),
      swMaxRuntimeSec_(DEFAULT_SW_MAX_SEC),
      pulsesPerGallon_(DEFAULT_PULSES_PER_GALLON) {
    for (uint8_t z = 0; z < ValveConfig::MAX_ZONES; ++z) {
        zoneDefaultSec_[z] = DEFAULT_RUN_SEC;
    }
}

void Persistence::begin() {
    // --- schema version + migration hook ---
    uint32_t ver = store_.getU32(KEY_SCHEMA, 0);
    if (ver == 0) {
        // Fresh install / empty NVS: stamp the current version, leave values at defaults.
        store_.putU32(KEY_SCHEMA, SCHEMA_VER);
    } else if (ver < SCHEMA_VER) {
        // Ordered, transforming migrations go here. V1 has none — the hook is installed,
        // not exercised. Additive fields never reach this branch (read-with-default).
        switch (ver) {
            // case 1: migrate v1 -> v2 ... (none yet)
            default: break;
        }
        store_.putU32(KEY_SCHEMA, SCHEMA_VER);
    }

    // --- load the mirror; every read falls back to its default when the key is absent ---
    char key[8];
    for (uint8_t z = 0; z < zoneCount_; ++z) {
        zoneDefaultSec_[z] = store_.getU32(zoneKey(key, z), DEFAULT_RUN_SEC);
    }
    swMaxRuntimeSec_ = store_.getU32(KEY_SW_MAX, DEFAULT_SW_MAX_SEC);

    pulsesPerGallon_ = store_.getFloat(KEY_K_PPG, DEFAULT_PULSES_PER_GALLON);
}

uint32_t Persistence::zoneDefaultSec(uint8_t zone) const {
    if (zone >= zoneCount_) return DEFAULT_RUN_SEC;
    return zoneDefaultSec_[zone];
}

void Persistence::setZoneDefaultSec(uint8_t zone, uint32_t sec) {
    if (zone >= zoneCount_) return;
    if (zoneDefaultSec_[zone] == sec) return;          // write-on-change: no-op if unchanged
    zoneDefaultSec_[zone] = sec;
    char key[8];
    store_.putU32(zoneKey(key, zone), sec);
}

void Persistence::setSwMaxRuntimeSec(uint32_t sec) {
    if (swMaxRuntimeSec_ == sec) return;
    swMaxRuntimeSec_ = sec;
    store_.putU32(KEY_SW_MAX, sec);
}

void Persistence::setPulsesPerGallon(float k) {
    if (k <= 0.0f) return;                 // a non-positive K is a calibration error; keep the old one
    if (pulsesPerGallon_ == k) return;     // write-on-change (exact: we only ever store what we set)
    pulsesPerGallon_ = k;
    store_.putFloat(KEY_K_PPG, k);
}

} // namespace tinkle

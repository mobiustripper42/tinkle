#include "persistence.h"
#include <stdio.h>   // snprintf for zone-key formatting

namespace tinkle {

// Static keys. All <= 15 chars (NVS hard limit — Preferences silently TRUNCATES longer
// keys, which would alias siblings). The per-zone key is generated in zoneKey().
static constexpr const char* KEY_SCHEMA  = "schema_ver";  // 10
static constexpr const char* KEY_SW_MAX  = "sw_max_sec";  // 10
static constexpr const char* KEY_K_PPG   = "k_ppg";       //  5  flow K, pulses/gallon (§7)
static constexpr const char* KEY_FLOWOVR = "flow_ovr";    //  8  DEC-015 override (#57)
static constexpr const char* KEY_SSID    = "wifi_ssid";   //  9  Phase 4 (#55)
static constexpr const char* KEY_PASS    = "wifi_pass";   //  9
static constexpr const char* KEY_SCHED   = "sched";       //  5  packed schedule blob (#56)
static constexpr const char* KEY_RUNLOG  = "runlog";      //  6  packed run-history blob (DEC-018)
static constexpr const char* KEY_FAULTLOG = "faultlog";   //  8  packed fault-log blob (#90)
static constexpr const char* KEY_DIST    = "dist";        //  4  Distributed Watering config (DEC-024)

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

    flowOverride_ = store_.getU8(KEY_FLOWOVR, 0) != 0;     // DEC-015: default = checks ON
    store_.getStr(KEY_SSID, wifiSsid_, SSID_CAP);          // "" when unconfigured
    store_.getStr(KEY_PASS, wifiPass_, PASS_CAP);

    // DEC-024: Distributed Watering config. Absent (torn/short) => disabled default, so a
    // controller that never enabled it reads exactly as before.
    uint8_t distBlob[DIST_CONFIG_BYTES];
    if (store_.getBytes(KEY_DIST, distBlob, sizeof(distBlob)) == DIST_CONFIG_BYTES)
        distributed_ = unpackDistributedConfig(distBlob);
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

void Persistence::setFlowOverride(bool on) {
    if (flowOverride_ == on) return;
    flowOverride_ = on;
    store_.putU8(KEY_FLOWOVR, on ? 1 : 0);
}

// Bounded copy that never relies on the source being shorter than the field.
static void copyCred(char* dst, uint16_t cap, const char* src) {
    uint16_t i = 0;
    if (src) for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// C-string equality up to the first NUL — array tails past the terminator are
// uninitialized in the staging buffers and must not vote.
static bool sameStr(const char* a, const char* b) {
    uint16_t i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

void Persistence::setWifiCreds(const char* ssid, const char* pass) {
    char newSsid[SSID_CAP]; copyCred(newSsid, SSID_CAP, ssid);
    char newPass[PASS_CAP]; copyCred(newPass, PASS_CAP, pass);
    if (sameStr(wifiSsid_, newSsid) && sameStr(wifiPass_, newPass)) return;   // write-on-change
    copyCred(wifiSsid_, SSID_CAP, newSsid);
    copyCred(wifiPass_, PASS_CAP, newPass);
    store_.putStr(KEY_SSID, wifiSsid_);
    store_.putStr(KEY_PASS, wifiPass_);
}

uint8_t Persistence::loadScheduleEntries(ScheduleEntry* out, uint8_t cap) {
    uint8_t blob[Scheduler::MAX_ENTRIES * SCHED_ENTRY_BYTES];
    const uint16_t len = store_.getBytes(KEY_SCHED, blob, sizeof(blob));
    uint8_t n = (uint8_t)(len / SCHED_ENTRY_BYTES);   // a torn/odd blob truncates safely
    if (n > cap) n = cap;
    for (uint8_t i = 0; i < n; ++i)
        out[i] = unpackScheduleEntry(blob + i * SCHED_ENTRY_BYTES);
    return n;
}

void Persistence::setDistributed(const DistributedConfig& c) {
    distributed_ = c;
    uint8_t blob[DIST_CONFIG_BYTES];
    packDistributedConfig(c, blob);           // whole record, replaced atomically (operator-rate)
    store_.putBytes(KEY_DIST, blob, DIST_CONFIG_BYTES);
}

void Persistence::saveScheduleEntries(const ScheduleEntry* entries, uint8_t count) {
    if (count > Scheduler::MAX_ENTRIES) count = Scheduler::MAX_ENTRIES;
    uint8_t blob[Scheduler::MAX_ENTRIES * SCHED_ENTRY_BYTES];
    for (uint8_t i = 0; i < count; ++i)
        packScheduleEntry(entries[i], blob + i * SCHED_ENTRY_BYTES);
    // The blob is the whole schedule, replaced atomically on every edit (§13
    // save-on-edit). No write-on-change diff: edits are operator-rate, not loop-rate.
    store_.putBytes(KEY_SCHED, blob, (uint16_t)(count * SCHED_ENTRY_BYTES));
}

void Persistence::loadRunLog(RunLog& log) {
    uint8_t blob[RUNLOG_BLOB_BYTES];
    const uint16_t len = store_.getBytes(KEY_RUNLOG, blob, sizeof(blob));  // 0 when absent
    log.deserialize(blob, len);                                           // empty ring on 0
}

void Persistence::saveRunLog(const RunLog& log) {
    uint8_t blob[RUNLOG_BLOB_BYTES];
    const uint16_t len = log.serialize(blob, sizeof(blob));
    // The blob is the whole ring, replaced atomically each write (DEC-018). No per-entry diff:
    // writes are run-rate, and main debounces them (write-on-change of the ring head).
    store_.putBytes(KEY_RUNLOG, blob, len);
}

void Persistence::loadFaultLog(FaultManager& fm) {
    uint8_t blob[FaultManager::BLOB_BYTES];
    const uint16_t len = store_.getBytes(KEY_FAULTLOG, blob, sizeof(blob));   // 0 when absent
    fm.deserialize(blob, len);                                               // empty ring on 0
}

void Persistence::saveFaultLog(const FaultManager& fm) {
    uint8_t blob[FaultManager::BLOB_BYTES];
    const uint16_t len = fm.serialize(blob, sizeof(blob));
    store_.putBytes(KEY_FAULTLOG, blob, len);                                // whole ring, atomic
}

} // namespace tinkle

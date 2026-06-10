#pragma once
#include <stdint.h>
#include "valve_driver.h"   // ValveConfig::MAX_ZONES — the single source of zone capacity

// Persistence — NVS/Preferences read/write of stored state (firmware spec §8, DEC-008).
//
// Platform-independent on purpose: it lives in src/core so it compiles for both the
// ESP32 firmware and the native host test runner. It touches no Arduino API directly —
// all flash I/O goes through an injected IKeyValueStore. The ESP32 build wraps
// Preferences (src/esp32/preferences_store.h); the host test runner uses an in-memory
// fake that counts writes (to prove write-on-change).
//
// SCOPE (DEC-008): this module persists the scalar config that exists today —
//   - per-zone manual default run durations (retires the BUTTON_RUN_SEC placeholder)
//   - swMaxRuntimeSec (software run ceiling, RunConfig)
//   - pulsesPerGallon (flow K, §7; loaded by FlowMonitor #34, written by calibration #36)
// No cached diverter position: the v1.4 two-leg NO/NC diverter has no hold-state to
// remember — the rest position is plain by construction, set per-run (DEC-013).
// The remaining §8 state is owned by modules not yet built; each fills the SAME store
// through its own keys when it lands — they are deliberately NOT pre-carved here:
//   - schedule entries        -> Scheduler (#27, §13 model)
//   - fertigation policy      -> Fert policy (#28)
//   - Wi-Fi credentials       -> Web/config (Phase 4)
//   - fault-log ring buffer   -> FaultManager (Phase 3/5)
//
// FORWARD-COMPAT (DEC-008): zones will be added after V1. Per-zone state uses
// zone-indexed keys ("z<N>_dur") read-with-default and iterated over the runtime
// zoneCount — adding a zone is "iterate further, default the missing key," needing no
// migration. A packed fixed-width-3-zone blob would force one; this does not.

namespace tinkle {

// The NVS abstraction. Typed key-value with read-with-default — mirrors Preferences'
// getUInt/putUInt/getUChar/putUChar surface, kept minimal to what §8's V1 scalars need.
struct IKeyValueStore {
    virtual uint32_t getU32(const char* key, uint32_t fallback) = 0;
    virtual void     putU32(const char* key, uint32_t value)    = 0;
    virtual uint8_t  getU8 (const char* key, uint8_t  fallback) = 0;
    virtual void     putU8 (const char* key, uint8_t  value)    = 0;
    virtual float    getFloat(const char* key, float fallback)  = 0;   // calibration K (§7)
    virtual void     putFloat(const char* key, float value)     = 0;
    virtual ~IKeyValueStore() = default;
};

class Persistence {
public:
    // Schema version of the on-flash layout. Bumped ONLY for a *transforming* migration
    // (a field whose meaning/encoding changes). Additive changes — a new zone, a new
    // defaulted scalar — are absorbed by read-with-default and do NOT bump this.
    static constexpr uint32_t SCHEMA_VER = 1;

    // First-boot / empty-NVS defaults.
    static constexpr uint32_t DEFAULT_RUN_SEC    = 600;   // retires BUTTON_RUN_SEC (main.cpp)
    static constexpr uint32_t DEFAULT_SW_MAX_SEC = 1200;  // matches RunConfig::swMaxRuntimeSec
    // Flow K (§7): a placeholder datasheet seed for the hall sensor — calibration (#36)
    // overwrites it. §15: defaults are seeds, not gospel; bench-confirm the real K.
    static constexpr float    DEFAULT_PULSES_PER_GALLON = 450.0f;

    // zoneCount is the runtime number of live zones (<= ValveConfig::MAX_ZONES). Injected,
    // not redefined — ValveConfig owns the count so the two never drift (DEC-008).
    Persistence(IKeyValueStore& store, uint8_t zoneCount);

    // Read schema_ver, run any migration (none for V1 — the hook is empty), then load
    // every key into the in-memory mirror. Empty/missing keys fall back to defaults
    // without faulting. Call once at boot after the store is open.
    void begin();

    // --- reads (the in-memory mirror; no flash hit) ---
    uint8_t  zoneCount()             const { return zoneCount_; }
    uint32_t zoneDefaultSec(uint8_t zone) const;   // out-of-range -> DEFAULT_RUN_SEC
    uint32_t swMaxRuntimeSec()       const { return swMaxRuntimeSec_; }
    float    pulsesPerGallon()       const { return pulsesPerGallon_; }   // flow K (§7, #34)

    // --- writes (write-on-change: a set to the current value touches no flash) ---
    void setZoneDefaultSec(uint8_t zone, uint32_t sec);
    void setSwMaxRuntimeSec(uint32_t sec);
    void setPulsesPerGallon(float k);         // written by calibration (#36); ignores k <= 0

private:
    // Build the per-zone key "z<N>_dur" into buf (capacity must be >= 8; NVS keys cap at
    // 15 chars and the longest this yields is "z15_dur" = 7). Returns buf.
    static const char* zoneKey(char* buf, uint8_t zone);

    IKeyValueStore& store_;
    uint8_t         zoneCount_;

    uint32_t zoneDefaultSec_[ValveConfig::MAX_ZONES];
    uint32_t swMaxRuntimeSec_;
    float    pulsesPerGallon_;
};

} // namespace tinkle

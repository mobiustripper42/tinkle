#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../core/persistence.h"

// ESP32 binding for IKeyValueStore — the only place persistence touches the Arduino
// Preferences (NVS) API. Header-only, mirroring arduino_gpio.h. One flat namespace
// ("tinkle") with prefixed keys (DEC-008); call begin() once before Persistence::begin().
//
// Preferences itself skips a flash write when the stored value is byte-identical, but we
// do not lean on that — Persistence's own write-on-change guard is the contract (§8).

namespace tinkle {

struct PreferencesStore : IKeyValueStore {
    Preferences prefs;

    void begin() { prefs.begin("tinkle", /*readOnly=*/false); }
    void end()   { prefs.end(); }

    uint32_t getU32(const char* key, uint32_t fallback) override { return prefs.getUInt(key, fallback); }
    void     putU32(const char* key, uint32_t value)    override { prefs.putUInt(key, value); }
    uint8_t  getU8 (const char* key, uint8_t  fallback) override { return prefs.getUChar(key, fallback); }
    void     putU8 (const char* key, uint8_t  value)    override { prefs.putUChar(key, value); }
};

} // namespace tinkle

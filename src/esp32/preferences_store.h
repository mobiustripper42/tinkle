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
    float    getFloat(const char* key, float fallback)  override { return prefs.getFloat(key, fallback); }
    void     putFloat(const char* key, float value)     override { prefs.putFloat(key, value); }

    bool getStr(const char* key, char* out, uint16_t cap) override {
        if (!cap) return false;
        out[0] = '\0';                      // a failed nvs_get_str writes nothing —
        if (!prefs.isKey(key)) return false;//   never hand back an unterminated buffer
        prefs.getString(key, out, cap);     // copies up to cap-1 + NUL
        return true;
    }
    void putStr(const char* key, const char* value) override { prefs.putString(key, value); }
    uint16_t getBytes(const char* key, void* out, uint16_t cap) override {
        if (!prefs.isKey(key)) return 0;
        return (uint16_t)prefs.getBytes(key, out, cap);
    }
    void putBytes(const char* key, const void* data, uint16_t len) override {
        prefs.putBytes(key, data, len);
    }
};

} // namespace tinkle

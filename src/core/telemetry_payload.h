#pragma once
#include <stddef.h>
#include <stdint.h>
#include "run_log.h"   // RunEntry / RunResult — core, dependency-free (DEC-018)

// Poop Deck telemetry (#13) — turn a logged run into the v1 irrigation message.
//
// tinkle is the "tinkle" event producer for Poop Deck (the farm telemetry store). One
// message per completed run, matching the canonical publisher docs/reference/tinkle_publish.ino
// in the poop-deck repo (its DEC-004 ingest contract: JSON, v-versioned, idempotent).
//
// Platform-independent on purpose (src/core, host-tested): it takes an ALREADY-formatted
// ISO-8601 timestamp string and does no time/timezone work — the esp32 shim owns TZ/DST
// (system_clock.h) and hands the string in. snprintf-built, so no ArduinoJson dependency.

namespace tinkle {

constexpr uint8_t POOPDECK_SCHEMA_V = 1;

// Publishable = a real run (result != None) with a trustworthy clock. ts_start is part of
// the store's natural key (source, zone, ts_start); a run logged before NTP synced
// (clockWasValid == false) has a meaningless timestamp and must never be published — it
// would defeat dedup. Non-publishable runs are dropped from telemetry, logged, not sent.
bool isPublishable(const RunEntry& e);

// "farm/irrigation/tinkle/zone<N>" — 1-based zone (zoneIndex + 1), matching the reference
// publisher and the synthetic data. Returns bytes written excl. NUL, 0 on overflow.
size_t buildIrrigationTopic(const RunEntry& e, char* out, size_t cap);

// Build the v1 JSON for a run into `out`, using the caller-formatted ISO-8601 timestamp
// `tsIso` (with an explicit UTC offset). Returns bytes written excl. NUL, or 0 if
// !isPublishable(e), tsIso is empty, or on overflow.
size_t buildIrrigationPayload(const RunEntry& e, const char* tsIso, char* out, size_t cap);

// Short, stable code for the payload "fault" field (NoFlow -> "no-flow", ...), or nullptr
// for Fault::None (0). Mirrors RunController's Fault enum; unknown codes -> "fault".
const char* faultCodeToString(uint8_t faultCode);

} // namespace tinkle

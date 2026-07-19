#include "poopdeck_publisher.h"
#include <time.h>
#include "../core/telemetry_payload.h"

namespace tinkle {

namespace {
constexpr uint16_t QUEUE_DEPTH   = 40;    // >= RUNLOG_DEPTH (32) backlog + live margin
constexpr uint16_t MQTT_BUFFER   = 512;   // payload ~130 B + topic — headroom
constexpr uint32_t RECONNECT_MS  = 5000;  // between connect attempts (task-local, blocking OK)
constexpr uint32_t IDLE_TICK_MS  = 500;   // queue-wait slice
constexpr uint32_t TASK_STACK    = 4096;  // words
constexpr UBaseType_t TASK_PRIO  = 1;
constexpr BaseType_t  TASK_CORE   = 0;    // PRO core; Arduino loop() runs on core 1
}  // namespace

void PoopdeckPublisher::begin(const Config& cfg, SystemClock& clock) {
    cfg_   = cfg;
    clock_ = &clock;
    queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(RunEntry));
    if (!queue_) { Serial.println(F("[poopdeck] queue alloc failed — telemetry disabled")); return; }
    mqtt_.setServer(cfg_.host, cfg_.port);
    mqtt_.setBufferSize(MQTT_BUFFER);
    xTaskCreatePinnedToCore(&PoopdeckPublisher::taskThunk, "poopdeck",
                            TASK_STACK, this, TASK_PRIO, nullptr, TASK_CORE);
    Serial.printf("[poopdeck] publisher task up -> %s:%u\n", cfg_.host, cfg_.port);
}

void PoopdeckPublisher::enqueue(const RunEntry& e) {
    if (!queue_ || !isPublishable(e)) return;         // drop pre-NTP / phantom runs quietly
    if (xQueueSend(queue_, &e, 0) != pdTRUE)
        Serial.println(F("[poopdeck] queue full — dropping a run (broker unreachable?)"));
}

void PoopdeckPublisher::enqueueBacklog(const RunLog& log) {
    // Oldest-first: at(count-1) is the oldest, at(0) the head. Replay in order so the store
    // lands them in history; ON CONFLICT makes any already-seen run a no-op.
    const uint8_t n = log.count();
    for (uint8_t i = n; i > 0; --i) enqueue(log.at(i - 1));
    if (n) Serial.printf("[poopdeck] queued %u run(s) from the boot ring for backfill\n", n);
}

void PoopdeckPublisher::taskThunk(void* self) {
    static_cast<PoopdeckPublisher*>(self)->run();
}

void PoopdeckPublisher::run() {
    for (;;) {
        // Only touch the queue while connected — a run stays buffered through an outage
        // (bounded by QUEUE_DEPTH; overflow is dropped-and-logged at enqueue).
        if (!mqtt_.connected()) {
            const uint32_t now = millis();
            if (WiFi.status() == WL_CONNECTED && (now - lastConnectMs_) >= RECONNECT_MS) {
                lastConnectMs_ = now;
                mqtt_.connect(cfg_.clientId, cfg_.user, cfg_.password);  // blocks; fine off-core
            }
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }
        mqtt_.loop();

        RunEntry e;
        if (xQueueReceive(queue_, &e, pdMS_TO_TICKS(IDLE_TICK_MS)) == pdTRUE) {
            if (!publishOne(e)) {
                // Publish missed (dropped connection mid-send, or clock briefly unsynced).
                // Put it back at the front so ordering holds; the loop reconnects and retries.
                if (xQueueSendToFront(queue_, &e, 0) != pdTRUE)
                    Serial.println(F("[poopdeck] requeue full — a run was lost"));
                vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            }
        }
    }
}

bool PoopdeckPublisher::publishOne(const RunEntry& e) {
    // tinkle stores LOCAL epoch (system_clock repacks UTC+offset). Recover the run's UTC by
    // removing the CURRENT offset (localNow - utcNow); publish a clean ...Z timestamp. Across
    // a DST change between run and publish this can be off by an hour for a backfilled run —
    // acceptable and rare; live runs publish seconds later at the same offset.
    const uint32_t utcNow = (uint32_t)time(nullptr);
    uint32_t localNow = 0;
    if (!clock_ || !clock_->localEpoch(localNow)) return false;   // clock not synced now — hold
    const int32_t  offset = (int32_t)localNow - (int32_t)utcNow;
    const time_t   utcRun = (time_t)((int32_t)e.startEpoch - offset);

    char ts[32];
    struct tm g;
    gmtime_r(&utcRun, &g);
    if (strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g) == 0) return false;

    char topic[48];
    char payload[256];
    if (buildIrrigationTopic(e, topic, sizeof(topic)) == 0) return false;
    if (buildIrrigationPayload(e, ts, payload, sizeof(payload)) == 0) return false;

    const bool ok = mqtt_.publish(topic, payload);
    if (ok) Serial.printf("[poopdeck] published %s\n", topic);
    return ok;
}

} // namespace tinkle

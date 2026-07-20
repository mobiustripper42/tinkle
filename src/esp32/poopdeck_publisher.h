#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../core/run_log.h"     // RunEntry / RunLog
#include "system_clock.h"        // SystemClock — local<->UTC (TZ lives in the esp32 shim)

// PoopdeckPublisher (#13) — ship completed runs to the Poop Deck MQTT broker as v1
// irrigation messages (docs/reference/tinkle_publish.ino / poop-deck DEC-004).
//
// The MQTT client runs on its OWN FreeRTOS task, pinned to the PRO core (0), OFF the
// Arduino/control core (1). PubSubClient's connect()/publish() block for up to seconds;
// running them on the control loop would blow the 10 ms budget (loop_monitor) and starve
// a valve — unacceptable on an irrigation controller. So the control loop only hands runs
// over a small queue (non-blocking enqueue); the task owns all networking. This is the
// same receive/persist decouple poop-deck itself uses (poop-deck DEC-006).

namespace tinkle {

class PoopdeckPublisher {
public:
    struct Config {
        const char* host;
        uint16_t    port;
        const char* clientId;
        const char* user;
        const char* password;
    };

    // Create the queue + background task. Call once in setup(), before any enqueue().
    // `clock` must outlive the publisher (it's a module-static in main).
    void begin(const Config& cfg, SystemClock& clock);

    // Hand a completed run to the publisher (non-blocking, control-loop-safe). No-op for a
    // non-publishable run (no real result / no valid clock). Dropped-and-logged if the
    // queue is full — bounded, logged loss (the store's own contract), never a block.
    void enqueue(const RunEntry& e);

    // Enqueue the persisted ring oldest-first, once at boot, so runs buffered while the
    // broker was unreachable replay on first connect. Idempotent on the store side.
    void enqueueBacklog(const RunLog& log);

private:
    static void taskThunk(void* self);
    void run();                         // the task loop (owns net_/mqtt_)
    bool publishOne(const RunEntry& e); // format ts + build + publish; false on any miss

    Config        cfg_{};
    SystemClock*  clock_  = nullptr;
    QueueHandle_t queue_  = nullptr;
    WiFiClient    net_;
    PubSubClient  mqtt_{net_};
    uint32_t      lastConnectMs_ = 0;
};

} // namespace tinkle

#include "valve_driver.h"

namespace tinkle {

ValveDriver::ValveDriver(IGpio& gpio, const ValveConfig& cfg)
    : g_(gpio), cfg_(cfg) {}

// Drive a bridge to Coast / Open / Close while guaranteeing the pair is never
// both HIGH — not even transiently. We write the side(s) that must be LOW first,
// then the single side (if any) that goes HIGH. Cmd has no both-high state, so
// the invariant is also unrepresentable at the call site.
void ValveDriver::applyBridge(const Bridge& b, Cmd cmd) {
    const bool h1 = (cmd == Cmd::Open);
    const bool h2 = (cmd == Cmd::Close);
    if (!h1) g_.write(b.in1, false);
    if (!h2) g_.write(b.in2, false);
    if (h1)  g_.write(b.in1, true);
    if (h2)  g_.write(b.in2, true);
}

void ValveDriver::begin() {
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z) {
        g_.configureOutput(cfg_.zones[z].in1);
        g_.configureOutput(cfg_.zones[z].in2);
        applyBridge(cfg_.zones[z], Cmd::Coast);
        zoneTimer_[z] = Timer{false, 0, 0};
    }
    g_.configureOutput(cfg_.diverter.in1);
    g_.configureOutput(cfg_.diverter.in2);
    applyBridge(cfg_.diverter, Cmd::Coast);
    divTimer_ = Timer{false, 0, 0};

    g_.configureOutput(cfg_.masterFet);
    g_.configureOutput(cfg_.pumpRelay);
    masterClose();
    pumpOff();
}

void ValveDriver::pulseOpen(uint8_t zone, uint32_t nowMs) {
    if (zone >= cfg_.zoneCount) return;
    applyBridge(cfg_.zones[zone], Cmd::Open);
    zoneTimer_[zone] = Timer{true, nowMs, cfg_.pulseMs};
}

void ValveDriver::pulseClose(uint8_t zone, uint32_t nowMs) {
    if (zone >= cfg_.zoneCount) return;
    applyBridge(cfg_.zones[zone], Cmd::Close);
    zoneTimer_[zone] = Timer{true, nowMs, cfg_.pulseMs};
}

void ValveDriver::setDiverter(bool through, uint32_t nowMs) {
    diverterThrough_ = through;
    diverterKnown_   = true;
    applyBridge(cfg_.diverter, through ? Cmd::Open : Cmd::Close);
    divTimer_ = Timer{true, nowMs, cfg_.diverterTravelMs};
}

void ValveDriver::masterOpen()  { g_.write(cfg_.masterFet, true);  masterOpen_ = true;  }
void ValveDriver::masterClose() { g_.write(cfg_.masterFet, false); masterOpen_ = false; }
void ValveDriver::pumpOn()      { g_.write(cfg_.pumpRelay, true);  pumpOn_ = true;  }
void ValveDriver::pumpOff()     { g_.write(cfg_.pumpRelay, false); pumpOn_ = false; }

void ValveDriver::safeState(uint32_t nowMs) {
    pumpOff();                                                  // pump off first
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z)
        pulseClose(z, nowMs);                                   // then close zones
    masterClose();                                              // then master closed
    // Diverter left as-is (§5).
}

void ValveDriver::tick(uint32_t nowMs) {
    // Unsigned subtraction so a millis() rollover doesn't strand a pulse HIGH.
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z) {
        Timer& t = zoneTimer_[z];
        if (t.active && (uint32_t)(nowMs - t.startMs) >= t.durMs) {
            applyBridge(cfg_.zones[z], Cmd::Coast);
            t.active = false;
        }
    }
    if (divTimer_.active && (uint32_t)(nowMs - divTimer_.startMs) >= divTimer_.durMs) {
        applyBridge(cfg_.diverter, Cmd::Coast);
        divTimer_.active = false;
    }
}

bool ValveDriver::zoneBusy(uint8_t zone) const {
    return zone < cfg_.zoneCount && zoneTimer_[zone].active;
}

bool ValveDriver::diverterBusy() const { return divTimer_.active; }

bool ValveDriver::busy() const {
    if (divTimer_.active) return true;
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z)
        if (zoneTimer_[z].active) return true;
    return false;
}

} // namespace tinkle

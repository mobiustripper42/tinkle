#include "valve_driver.h"

namespace tinkle {

ValveDriver::ValveDriver(IGpio& gpio, const ValveConfig& cfg)
    : g_(gpio), cfg_(cfg) {}

// Safety contract for boot: LOW is the safe/rest level for every FET — NC zones
// closed, NC fert-leg closed, NO bypass open (plain), pump off. configureOutput
// (pinMode OUTPUT) drives a pin to its default before the explicit write lands, so
// every actuator being LOW=safe is what makes boot fail-dry. There are no HIGH-safe
// pins to glitch. (The hardware gate-to-GND pulldowns hold the FETs off through the
// boot strap window regardless — see the wiring doc.)
void ValveDriver::begin() {
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z) {
        g_.configureOutput(cfg_.zoneFet[z]);
        g_.write(cfg_.zoneFet[z], false);
        zoneTimer_[z] = Timer{false, 0, 0};
    }
    g_.configureOutput(cfg_.divCleanFet);
    g_.configureOutput(cfg_.divFertFet);
    g_.configureOutput(cfg_.pumpRelay);
    pumpOff();
    g_.write(cfg_.divCleanFet, false);   // both legs to rest => plain
    g_.write(cfg_.divFertFet,  false);
    diverterFert_ = false;
    divTimer_ = Timer{false, 0, 0};
}

void ValveDriver::openZone(uint8_t zone, uint32_t nowMs) {
    if (zone >= cfg_.zoneCount) return;
    g_.write(cfg_.zoneFet[zone], true);                 // energize -> NC valve drives open
    zoneTimer_[zone] = Timer{true, nowMs, cfg_.zoneTravelMs};
}

void ValveDriver::closeZone(uint8_t zone, uint32_t nowMs) {
    if (zone >= cfg_.zoneCount) return;
    g_.write(cfg_.zoneFet[zone], false);                // de-energize -> cap auto-return closes
    zoneTimer_[zone] = Timer{true, nowMs, cfg_.zoneTravelMs};
}

void ValveDriver::setDiverter(bool fertigate, uint32_t nowMs) {
    diverterFert_ = fertigate;
    g_.write(cfg_.divFertFet,  fertigate);   // NC fert leg:  high = open
    g_.write(cfg_.divCleanFet, fertigate);   // NO bypass leg: high = closed
    divTimer_ = Timer{true, nowMs, cfg_.diverterTravelMs};
}

void ValveDriver::pumpOn()  { g_.write(cfg_.pumpRelay, true);  pumpOn_ = true;  }
void ValveDriver::pumpOff() { g_.write(cfg_.pumpRelay, false); pumpOn_ = false; }

void ValveDriver::safeState(uint32_t /*nowMs*/) {
    pumpOff();                                          // source off first — the fail-dry gate
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z) {      // then de-energize the zones (cap-close)
        g_.write(cfg_.zoneFet[z], false);
        zoneTimer_[z].active = false;
    }
    g_.write(cfg_.divCleanFet, false);                  // diverter returns to plain (§5)
    g_.write(cfg_.divFertFet,  false);
    diverterFert_ = false;
    divTimer_.active = false;
}

void ValveDriver::tick(uint32_t nowMs) {
    // Unsigned subtraction so a millis() rollover doesn't strand a timer. Only the
    // busy flag clears here — the FET level holds (an open valve stays energized).
    for (uint8_t z = 0; z < cfg_.zoneCount; ++z) {
        Timer& t = zoneTimer_[z];
        if (t.active && (uint32_t)(nowMs - t.startMs) >= t.durMs) t.active = false;
    }
    if (divTimer_.active && (uint32_t)(nowMs - divTimer_.startMs) >= divTimer_.durMs)
        divTimer_.active = false;
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

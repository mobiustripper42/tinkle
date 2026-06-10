#include "fault_manager.h"

namespace tinkle {

void FaultManager::tick(uint32_t nowMs) {
    // First-fault-wins in RunController means the only edges are None -> X
    // (latch) and X -> None (clear); log the latch edge.
    const Fault f = rc_.activeFault();
    if (f != Fault::None && f != prevFault_) append(f, nowMs);
    prevFault_ = f;
}

void FaultManager::note(Fault code, uint32_t nowMs) {
    if (code == Fault::None) return;
    append(code, nowMs);
}

void FaultManager::append(Fault code, uint32_t nowMs) {
    log_[logHead_].code = code;
    log_[logHead_].atMs = nowMs;
    logHead_ = (uint8_t)((logHead_ + 1) % LOG_SIZE);
    if (logCount_ < LOG_SIZE) ++logCount_;
}

void FaultManager::setConditionActive(Fault code, bool active) {
    if (idx(code) >= CODE_COUNT || code == Fault::None) return;
    condActive_[idx(code)] = active;
}

bool FaultManager::conditionActive(Fault code) const {
    if (idx(code) >= CODE_COUNT || code == Fault::None) return false;
    return condActive_[idx(code)];
}

bool FaultManager::clearAllowed() const {
    return rc_.isFaulted() && !conditionActive(rc_.activeFault());
}

bool FaultManager::requestClear() {
    if (!clearAllowed()) return false;
    const bool cleared = rc_.clearFault();
    // Sync the edge detector here rather than waiting for the next tick(): a
    // re-raise of the SAME code inside the current tick window must still log.
    if (cleared) prevFault_ = Fault::None;
    return cleared;
}

FaultManager::Entry FaultManager::logEntry(uint8_t i) const {
    if (i >= logCount_) return Entry{};
    // newest = head-1, walk backwards.
    const uint8_t slot = (uint8_t)((logHead_ + LOG_SIZE - 1 - i) % LOG_SIZE);
    return log_[slot];
}

} // namespace tinkle

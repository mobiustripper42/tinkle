#include "fault_manager.h"

namespace tinkle {

void FaultManager::tick(uint32_t epoch, bool clockValid) {
    // First-fault-wins in RunController means the only edges are None -> X
    // (latch) and X -> None (clear); log the latch edge.
    const Fault f = rc_.activeFault();
    if (f != Fault::None && f != prevFault_) append(f, epoch, clockValid);
    prevFault_ = f;
}

void FaultManager::note(Fault code, uint32_t epoch, bool clockValid) {
    if (code == Fault::None) return;
    append(code, epoch, clockValid);
}

void FaultManager::append(Fault code, uint32_t epoch, bool clockValid) {
    log_[logHead_].code          = code;
    log_[logHead_].epoch         = epoch;
    // Pre-2025 epoch-sanity guard (#90, shared threshold with the runlog): a pre-NTP epoch is
    // stored but flagged invalid so the SPA renders "unsynced", never a 1970 wall-clock.
    log_[logHead_].clockWasValid = clockValid && epoch >= RUNLOG_MIN_VALID_EPOCH;
    logHead_ = (uint8_t)((logHead_ + 1) % LOG_SIZE);
    if (logCount_ < LOG_SIZE) ++logCount_;
    dirty_ = true;
}

// 6-byte record: epoch u32 LE | code u8 | flags u8 (bit0 clockWasValid). Mirrors the runlog
// packing style (little-endian, padding-free).
static void packFaultEntry(const FaultManager::Entry& e, uint8_t* o) {
    o[0] = (uint8_t)(e.epoch & 0xFF);
    o[1] = (uint8_t)((e.epoch >> 8)  & 0xFF);
    o[2] = (uint8_t)((e.epoch >> 16) & 0xFF);
    o[3] = (uint8_t)((e.epoch >> 24) & 0xFF);
    o[4] = (uint8_t)e.code;
    o[5] = e.clockWasValid ? 0x01 : 0x00;
}

uint16_t FaultManager::serialize(uint8_t* out, uint16_t cap) const {
    uint16_t bytes = 0;
    for (uint8_t k = 0; k < logCount_; ++k) {            // oldest -> newest
        if (bytes + ENTRY_BYTES > cap) break;
        const Entry e = logEntry((uint8_t)(logCount_ - 1 - k));
        packFaultEntry(e, out + bytes);
        bytes += ENTRY_BYTES;
    }
    return bytes;
}

void FaultManager::deserialize(const uint8_t* in, uint16_t len) {
    logHead_ = 0;
    logCount_ = 0;
    uint16_t n = len / ENTRY_BYTES;                      // whole entries only
    uint16_t start = 0;
    if (n > LOG_SIZE) { start = n - LOG_SIZE; n = LOG_SIZE; }   // keep the last LOG_SIZE
    for (uint16_t i = start; i < start + n; ++i) {
        const uint8_t* p = in + (uint32_t)i * ENTRY_BYTES;
        Entry e;
        e.epoch         = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        e.code          = (Fault)p[4];
        e.clockWasValid = (p[5] & 0x01) != 0;
        log_[logHead_] = e;
        logHead_ = (uint8_t)((logHead_ + 1) % LOG_SIZE);
        if (logCount_ < LOG_SIZE) ++logCount_;
    }
    dirty_ = false;                                      // rehydrate is not a change
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

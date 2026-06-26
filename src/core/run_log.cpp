#include "run_log.h"

namespace tinkle {

void packRunEntry(const RunEntry& e, uint8_t out[RUNLOG_ENTRY_BYTES]) {
    out[0] = (uint8_t)(e.startEpoch & 0xFF);            // u32 little-endian
    out[1] = (uint8_t)((e.startEpoch >> 8)  & 0xFF);
    out[2] = (uint8_t)((e.startEpoch >> 16) & 0xFF);
    out[3] = (uint8_t)((e.startEpoch >> 24) & 0xFF);
    out[4] = e.zoneIndex;
    out[5] = (uint8_t)(e.durationSec & 0xFF);           // u16 little-endian
    out[6] = (uint8_t)(e.durationSec >> 8);
    out[7] = (uint8_t)(e.centigallons & 0xFF);          // u16 little-endian
    out[8] = (uint8_t)(e.centigallons >> 8);
    out[9] = (uint8_t)((e.fertigate ? 0x01 : 0x00)              // bit0
                       | (((uint8_t)e.result & 0x03) << 1)      // bits1-2
                       | (e.clockWasValid ? 0x08 : 0x00));      // bit3
    out[10] = e.faultCode;
}

RunEntry unpackRunEntry(const uint8_t in[RUNLOG_ENTRY_BYTES]) {
    RunEntry e;
    e.startEpoch    = (uint32_t)in[0]
                    | ((uint32_t)in[1] << 8)
                    | ((uint32_t)in[2] << 16)
                    | ((uint32_t)in[3] << 24);
    e.zoneIndex     = in[4];
    e.durationSec   = (uint16_t)(in[5] | ((uint16_t)in[6] << 8));
    e.centigallons  = (uint16_t)(in[7] | ((uint16_t)in[8] << 8));
    const uint8_t flags = in[9];
    e.fertigate     = (flags & 0x01) != 0;
    e.result        = (RunResult)((flags >> 1) & 0x03);
    e.clockWasValid = (flags & 0x08) != 0;
    e.faultCode     = in[10];
    return e;
}

void RunLog::push(const RunEntry& e) {
    if (count_ == 0) {
        headIdx_ = 0;
        count_   = 1;
    } else {
        headIdx_ = (uint8_t)((headIdx_ + 1) % RUNLOG_DEPTH);
        if (count_ < RUNLOG_DEPTH) ++count_;            // saturate at full; head laps the tail
    }
    ring_[headIdx_] = e;
}

const RunEntry& RunLog::head() const {
    static const RunEntry kEmpty;                       // result=None for a never-run ring
    return count_ == 0 ? kEmpty : ring_[headIdx_];
}

RunEntry RunLog::at(uint8_t i) const {
    if (i >= count_) return RunEntry{};
    const uint8_t idx = (uint8_t)((headIdx_ + RUNLOG_DEPTH - i) % RUNLOG_DEPTH);
    return ring_[idx];
}

uint16_t RunLog::serialize(uint8_t* out, uint16_t cap) const {
    uint16_t bytes = 0;
    // Oldest (at count-1) -> newest (at 0): replaying these through push() rebuilds the ring
    // with the head landing on the newest entry.
    for (uint8_t k = 0; k < count_; ++k) {
        if (bytes + RUNLOG_ENTRY_BYTES > cap) break;    // truncate to whole entries
        const RunEntry e = at((uint8_t)(count_ - 1 - k));
        packRunEntry(e, out + bytes);
        bytes += RUNLOG_ENTRY_BYTES;
    }
    return bytes;
}

void RunLog::deserialize(const uint8_t* in, uint16_t len) {
    headIdx_ = 0;
    count_   = 0;
    uint16_t n = len / RUNLOG_ENTRY_BYTES;              // whole entries only; a torn tail drops
    uint16_t start = 0;
    if (n > RUNLOG_DEPTH) { start = n - RUNLOG_DEPTH; n = RUNLOG_DEPTH; }  // keep the last depth
    for (uint16_t i = start; i < start + n; ++i)
        push(unpackRunEntry(in + (uint32_t)i * RUNLOG_ENTRY_BYTES));
}

} // namespace tinkle

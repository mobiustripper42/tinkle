#include "loop_monitor.h"

namespace tinkle {

bool LoopMonitor::record(uint32_t durUs) {
    s_.ticks++;
    s_.lastUs = durUs;
    if (durUs > s_.maxUs) s_.maxUs = durUs;
    const bool over = durUs > budgetUs_;
    if (over) s_.overruns++;
    return over;
}

} // namespace tinkle

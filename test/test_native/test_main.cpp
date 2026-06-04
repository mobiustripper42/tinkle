#include <unity.h>
#include <map>
#include <vector>
#include <utility>
#include "valve_driver.h"
#include "run_controller.h"
#include "loop_monitor.h"

// Native unit tests for ValveDriver (firmware spec §5). Fake GPIO + an explicit
// clock value (no millis()): the host-testable tier per CLAUDE.md.
//
// The fake doesn't just record writes — it knows the H-bridge pairs and trips a
// violation flag the instant any registered pair is driven both-HIGH. That makes
// the never-both-high invariant a tested property at write granularity, not an
// assertion checked only after the dust settles.

using tinkle::Bridge;
using tinkle::IGpio;
using tinkle::ValveConfig;
using tinkle::ValveDriver;

namespace {

struct FakeGpio : IGpio {
    std::map<uint8_t, bool> level;        // current commanded level per pin
    std::map<uint8_t, bool> configured;   // pins set as outputs
    std::vector<std::pair<uint8_t, bool>> log;  // ordered write history
    std::vector<Bridge> bridges;          // pairs to police
    bool bothHighViolation = false;

    void watch(Bridge b) { bridges.push_back(b); }

    void configureOutput(uint8_t pin) override { configured[pin] = true; }

    void write(uint8_t pin, bool high) override {
        level[pin] = high;
        log.push_back({pin, high});
        for (const auto& b : bridges) {
            if (pin == b.in1 || pin == b.in2) {
                if (level.count(b.in1) && level.count(b.in2) &&
                    level[b.in1] && level[b.in2]) {
                    bothHighViolation = true;
                }
            }
        }
    }

    int writesTo(uint8_t pin) const {
        int n = 0;
        for (const auto& w : log) if (w.first == pin) ++n;
        return n;
    }
    // Index into log of the last write to pin, or -1.
    int lastIndexOf(uint8_t pin) const {
        int idx = -1;
        for (int i = 0; i < (int)log.size(); ++i) if (log[i].first == pin) idx = i;
        return idx;
    }
};

// Synthetic pin map — values are arbitrary, just distinct.
constexpr Bridge Z0{2, 3};
constexpr Bridge Z1{4, 5};
constexpr Bridge DIV{6, 7};   // in1 = THROUGH (fertigate), in2 = AROUND (plain)
constexpr uint8_t MASTER = 8;
constexpr uint8_t PUMP   = 9;

ValveConfig makeConfig() {
    ValveConfig c;
    c.zones[0] = Z0;
    c.zones[1] = Z1;
    c.zoneCount = 2;
    c.diverter = DIV;
    c.masterFet = MASTER;
    c.pumpRelay = PUMP;
    c.pulseMs = 75;
    c.diverterTravelMs = 6000;
    return c;
}

FakeGpio g;
ValveConfig cfg;

void watchAll() { g.watch(Z0); g.watch(Z1); g.watch(DIV); }

} // namespace

void setUp() {
    g = FakeGpio{};
    cfg = makeConfig();
    watchAll();
}
void tearDown() {}

// begin() forces the safe state: bridges coast, master closed, pump off.
static void test_begin_safe_levels() {
    ValveDriver vd(g, cfg);
    vd.begin();
    TEST_ASSERT_FALSE(g.level[Z0.in1]); TEST_ASSERT_FALSE(g.level[Z0.in2]);
    TEST_ASSERT_FALSE(g.level[Z1.in1]); TEST_ASSERT_FALSE(g.level[Z1.in2]);
    TEST_ASSERT_FALSE(g.level[DIV.in1]); TEST_ASSERT_FALSE(g.level[DIV.in2]);
    TEST_ASSERT_FALSE(g.level[MASTER]);
    TEST_ASSERT_FALSE(g.level[PUMP]);
    TEST_ASSERT_TRUE(g.configured[Z0.in1] && g.configured[PUMP] && g.configured[MASTER]);
    TEST_ASSERT_FALSE(g.bothHighViolation);
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
}

// pulseOpen drives IN1 high / IN2 low, holds for pulseMs, then coasts both low.
static void test_pulse_open_then_coast() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pulseOpen(0, 1000);
    TEST_ASSERT_TRUE(g.level[Z0.in1]);
    TEST_ASSERT_FALSE(g.level[Z0.in2]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    TEST_ASSERT_TRUE(vd.busy());

    vd.tick(1000 + 74);                 // one ms short of pulseMs — still driving
    TEST_ASSERT_TRUE(g.level[Z0.in1]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));

    vd.tick(1000 + 75);                 // pulseMs elapsed — coast
    TEST_ASSERT_FALSE(g.level[Z0.in1]);
    TEST_ASSERT_FALSE(g.level[Z0.in2]);
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(vd.busy());
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// pulseClose is the mirror: IN2 high / IN1 low, then coast.
static void test_pulse_close_then_coast() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pulseClose(1, 500);
    TEST_ASSERT_FALSE(g.level[Z1.in1]);
    TEST_ASSERT_TRUE(g.level[Z1.in2]);
    vd.tick(500 + 75);
    TEST_ASSERT_FALSE(g.level[Z1.in2]);
    TEST_ASSERT_FALSE(vd.zoneBusy(1));
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// Re-pulsing the opposite direction without coasting first must never put both
// inputs HIGH — the LOW-side-first ordering guards the reversal.
static void test_never_both_high_on_reversal() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pulseOpen(0, 0);     // IN1 high
    vd.pulseClose(0, 10);   // immediate reversal: IN1 must drop before IN2 rises
    TEST_ASSERT_FALSE(g.level[Z0.in1]);
    TEST_ASSERT_TRUE(g.level[Z0.in2]);
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// Independent per-actuator timers: a 6 s diverter travel must not hold up a
// 75 ms zone pulse, and the zone releasing must not cut the diverter short.
static void test_independent_timers() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pulseOpen(0, 0);
    vd.setDiverter(true, 10);          // THROUGH, 6000 ms travel
    TEST_ASSERT_TRUE(g.level[DIV.in1]);
    TEST_ASSERT_TRUE(vd.diverterThrough());

    vd.tick(80);                       // zone pulse done; diverter still traveling
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(g.level[Z0.in1]);
    TEST_ASSERT_TRUE(vd.diverterBusy());
    TEST_ASSERT_TRUE(g.level[DIV.in1]);

    vd.tick(10 + 6000);                // travel window elapsed
    TEST_ASSERT_FALSE(vd.diverterBusy());
    TEST_ASSERT_FALSE(g.level[DIV.in1]);
    TEST_ASSERT_FALSE(g.level[DIV.in2]);
    TEST_ASSERT_FALSE(vd.busy());
}

// Reversing the diverter mid-travel must never drive its bridge both-HIGH. (The
// electrical dead-time question for a motor reversed mid-travel is separate and
// flagged to @architect — this guards only the logical invariant.)
static void test_diverter_reversal_never_both_high() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.setDiverter(true, 0);            // THROUGH, IN1 high
    vd.setDiverter(false, 100);         // reverse to AROUND mid-travel
    TEST_ASSERT_FALSE(g.level[DIV.in1]);
    TEST_ASSERT_TRUE(g.level[DIV.in2]);
    TEST_ASSERT_FALSE(vd.diverterThrough());
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// safeState landing while a zone is mid-open-pulse (the §14 fault case): the open
// is overridden by a close pulse, with no transient both-high.
static void test_safe_state_during_open_pulse() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pulseOpen(0, 0);                 // zone 0 mid-open
    TEST_ASSERT_TRUE(g.level[Z0.in1]);
    vd.safeState(30);                  // fault unwind before the open pulse expires
    TEST_ASSERT_FALSE(g.level[Z0.in1]);
    TEST_ASSERT_TRUE(g.level[Z0.in2]);  // now driving closed
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(g.bothHighViolation);
    vd.tick(30 + 75);                   // close pulse completes -> coast
    TEST_ASSERT_FALSE(g.level[Z0.in2]);
    TEST_ASSERT_FALSE(vd.busy());
}

// Diverter direction mapping: through => IN1 (THROUGH), !through => IN2 (AROUND).
static void test_diverter_direction() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.setDiverter(false, 0);          // AROUND
    TEST_ASSERT_FALSE(g.level[DIV.in1]);
    TEST_ASSERT_TRUE(g.level[DIV.in2]);
    TEST_ASSERT_FALSE(vd.diverterThrough());
    TEST_ASSERT_TRUE(vd.diverterKnown());
}

// Master FET + pump relay are immediate level sets with matching state getters.
static void test_master_and_pump() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.masterOpen(); TEST_ASSERT_TRUE(g.level[MASTER]); TEST_ASSERT_TRUE(vd.masterIsOpen());
    vd.pumpOn();     TEST_ASSERT_TRUE(g.level[PUMP]);   TEST_ASSERT_TRUE(vd.pumpIsOn());
    vd.pumpOff();    TEST_ASSERT_FALSE(g.level[PUMP]);  TEST_ASSERT_FALSE(vd.pumpIsOn());
    vd.masterClose();TEST_ASSERT_FALSE(g.level[MASTER]);TEST_ASSERT_FALSE(vd.masterIsOpen());
}

// safeState: pump off, both zones close-pulsed, master closed — and the pump is
// commanded off BEFORE the master is closed (§14 unwind order). Diverter untouched.
static void test_safe_state() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.masterOpen();
    vd.pumpOn();
    int divWritesBefore = g.writesTo(DIV.in1) + g.writesTo(DIV.in2);

    vd.safeState(2000);

    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_TRUE(g.level[Z0.in2]);   // zones driven closed
    TEST_ASSERT_TRUE(g.level[Z1.in2]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    TEST_ASSERT_TRUE(vd.zoneBusy(1));
    // pump-off precedes master-close in the write log
    TEST_ASSERT_TRUE(g.lastIndexOf(PUMP) < g.lastIndexOf(MASTER));
    // diverter not commanded by safeState
    TEST_ASSERT_EQUAL_INT(divWritesBefore, g.writesTo(DIV.in1) + g.writesTo(DIV.in2));
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// Out-of-range zone is a no-op (no writes, no crash).
static void test_zone_bounds() {
    ValveDriver vd(g, cfg);
    vd.begin();
    int before = (int)g.log.size();
    vd.pulseOpen(7, 0);
    vd.pulseClose(7, 0);
    TEST_ASSERT_EQUAL_INT(before, (int)g.log.size());
    TEST_ASSERT_FALSE(vd.zoneBusy(7));
}

// A millis() rollover mid-pulse must still release on time (unsigned math).
static void test_millis_rollover() {
    ValveDriver vd(g, cfg);
    vd.begin();
    const uint32_t start = 0xFFFFFFF0;   // 16 ms before wrap
    vd.pulseOpen(0, start);
    vd.tick(start + 40);                 // 40 ms elapsed (wrapped past 0) — still < 75
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    vd.tick(start + 75);                 // 75 ms elapsed across the wrap — release
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(g.level[Z0.in1]);
}

// ----------------------------------------------------------------------------
// RunController (firmware spec §4). Same FakeGpio (so the never-both-high
// invariant is policed across whole runs), a real ValveDriver, and an explicit
// clock. Faults are injected via raiseFault(), matching how FlowMonitor/Watchdog
// will notify it later.
// ----------------------------------------------------------------------------

using tinkle::Fault;
using tinkle::RunConfig;
using tinkle::RunController;
using tinkle::RunRequest;
using tinkle::RunState;
using tinkle::RunSummary;

namespace {

RunConfig makeRunCfg() {
    RunConfig rc;
    rc.settleMs = 100;          // keep the dwell short for tests
    rc.swMaxRuntimeSec = 1200;
    return rc;
}

// Pump the cooperative loop: tick, advancing the clock by stepMs each time, until
// pred() holds or the budget is spent. Returns the final clock value. Mirrors the
// real main loop driving runController.tick() — instantaneous states advance one
// tick at a time, timed states resolve as the clock moves.
template <class Pred>
uint32_t pump(RunController& rc, uint32_t now, uint32_t stepMs, uint32_t budgetMs, Pred pred) {
    uint32_t spent = 0;
    while (true) {
        if (pred()) break;
        rc.tick(now);
        if (spent >= budgetMs) break;
        now += stepMs;
        spent += stepMs;
    }
    return now;
}

} // namespace

// begin() drives the safe state and lands in IDLE.
static void test_rc_begin_idle_safe() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_EQUAL_INT(-1, rc.activeZone());
}

// Full happy-path run: PREP -> ... -> RUNNING for the duration -> back to IDLE,
// pump and master off, logged Completed, invariant never violated.
static void test_rc_full_sequence_completes() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 2; req.fertigate = false;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));

    // Reach RUNNING (first run travels the diverter — position unknown).
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_TRUE(vd.masterIsOpen());
    TEST_ASSERT_TRUE(vd.pumpIsOn());
    TEST_ASSERT_EQUAL_INT(0, rc.activeZone());
    TEST_ASSERT_TRUE(rc.remainingSec(now) >= 1 && rc.remainingSec(now) <= 2);

    // Run out the 2 s duration + settle -> IDLE.
    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_EQUAL(RunSummary::Result::Completed, rc.lastRun().result);
    TEST_ASSERT_EQUAL_UINT8(0, rc.lastRun().zone);
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// The diverter only travels when the position must change (§6). A first fert run
// establishes THROUGH; a second fert run must NOT re-drive the diverter.
static void test_rc_diverter_skip_when_unchanged() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1; req.fertigate = true;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(vd.diverterThrough());
    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });

    int divWritesBefore = g.writesTo(DIV.in1) + g.writesTo(DIV.in2);
    rc.requestRun(req, now);    // same fert state -> no travel
    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL_INT(divWritesBefore, g.writesTo(DIV.in1) + g.writesTo(DIV.in2));
    TEST_ASSERT_TRUE(vd.diverterThrough());
}

// Graceful stop() mid-run unwinds to IDLE with pump+master off; logged Stopped.
static void test_rc_stop_unwinds() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 1; req.durationSec = 30; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });

    rc.stop(now);
    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_EQUAL(RunSummary::Result::Stopped, rc.lastRun().result);
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// A fault mid-run slams the safe state and latches (§14). Further runs are
// refused until clearFault(), after which a normal run is possible again.
static void test_rc_fault_latches_and_clears() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 30; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });

    rc.raiseFault(Fault::NoFlow, now);
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_TRUE(rc.isFaulted());
    TEST_ASSERT_EQUAL(Fault::NoFlow, rc.activeFault());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_EQUAL(RunSummary::Result::Faulted, rc.lastRun().result);

    // Refused while latched.
    TEST_ASSERT_FALSE(rc.requestRun(req, now));

    // Clear, then a fresh run reaches RUNNING.
    TEST_ASSERT_TRUE(rc.clearFault());
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_TRUE(rc.requestRun(req, now));
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// Unexpected flow during IDLE faults straight to safe state (§14 idle case).
static void test_rc_fault_from_idle() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    rc.raiseFault(Fault::UnexpectedFlow, 500);
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow, rc.activeFault());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
}

// Queued requests run sequentially: zone 0 then zone 1.
static void test_rc_queue_runs_sequentially() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest a; a.zoneIndex = 0; a.durationSec = 1; a.fertigate = false;
    RunRequest b; b.zoneIndex = 1; b.durationSec = 1; b.fertigate = false;
    TEST_ASSERT_TRUE(rc.requestRun(a, 0));
    TEST_ASSERT_TRUE(rc.requestRun(b, 0));      // queued behind a
    TEST_ASSERT_EQUAL_UINT8(1, rc.queueDepth());

    // a runs first.
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL_INT(0, rc.activeZone());

    // b becomes the active run on zone 1.
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running && rc.activeZone() == 1; });
    TEST_ASSERT_EQUAL_INT(1, rc.activeZone());
    TEST_ASSERT_EQUAL_UINT8(0, rc.queueDepth());

    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// Bad requests are rejected: out-of-range zone, zero duration.
static void test_rc_rejects_bad_requests() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest badZone; badZone.zoneIndex = 9; badZone.durationSec = 5;
    RunRequest zeroDur; zeroDur.zoneIndex = 0; zeroDur.durationSec = 0;
    TEST_ASSERT_FALSE(rc.requestRun(badZone, 0));
    TEST_ASSERT_FALSE(rc.requestRun(zeroDur, 0));
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
}

// The software ceiling caps an oversized run: a 1000 s request with swMax=1 s
// stops after ~1 s, not 1000.
static void test_rc_sw_max_runtime_clamps() {
    ValveDriver vd(g, cfg);
    RunConfig rc_cfg = makeRunCfg();
    rc_cfg.swMaxRuntimeSec = 1;          // 1 s ceiling
    RunController rc(vd, rc_cfg);
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 1000; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(rc.remainingSec(now) <= 1);
    // Within a couple seconds of ticking it must have stopped — far short of 1000 s.
    now = pump(rc, now, 5, 3000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
}

// §4 step 2: a watchdog trip asserted before OPEN_MASTER aborts to FAULT and the
// master is NEVER energized.
static void test_rc_watchdog_pre_open_gate() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    rc.setWatchdogTripped(true);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.isFaulted(); });
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_EQUAL(Fault::Watchdog, rc.activeFault());
    TEST_ASSERT_FALSE(vd.masterIsOpen());        // never opened water
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    (void)now;
}

// A fault landing while the diverter is mid-travel (before the master ever opens)
// still slams the safe state, never opens the master, and holds the invariant.
static void test_rc_fault_during_prep() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5; req.fertigate = true;
    rc.requestRun(req, 0);
    rc.tick(0);
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());
    TEST_ASSERT_TRUE(vd.diverterBusy());

    rc.raiseFault(Fault::NoFlow, 100);
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_FALSE(vd.masterIsOpen());        // master never opened during prep
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(g.bothHighViolation);
}

// stop() during PREP_DIVERTER unwinds to IDLE while the diverter coasts in the
// background — no both-high, pump+master safe.
static void test_rc_stop_during_prep() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5; req.fertigate = true;
    rc.requestRun(req, 0);
    rc.tick(0);
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());

    rc.stop(100);
    uint32_t now = pump(rc, 100, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.masterIsOpen());
    TEST_ASSERT_FALSE(g.bothHighViolation);
    (void)now;
}

// The queue holds at most MAX_QUEUE behind the active run; the overflow request
// is rejected.
static void test_rc_queue_full_rejected() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5; req.fertigate = false;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));     // active
    for (int i = 0; i < RunConfig::MAX_QUEUE; ++i)
        TEST_ASSERT_TRUE(rc.requestRun(req, 0)); // fill the queue
    TEST_ASSERT_EQUAL_UINT8(RunConfig::MAX_QUEUE, rc.queueDepth());
    TEST_ASSERT_FALSE(rc.requestRun(req, 0));    // overflow rejected
}

// --- LoopMonitor (firmware spec §2 tick budget) ---------------------------------

using tinkle::LoopMonitor;
using tinkle::LoopStats;

void test_loop_monitor_within_budget() {
    LoopMonitor m(10000);   // 10 ms budget
    TEST_ASSERT_FALSE(m.record(120));
    TEST_ASSERT_FALSE(m.record(9999));   // boundary: equal-or-under is not an overrun
    TEST_ASSERT_FALSE(m.record(10000));
    const LoopStats& s = m.stats();
    TEST_ASSERT_EQUAL_UINT32(3, s.ticks);
    TEST_ASSERT_EQUAL_UINT32(0, s.overruns);
    TEST_ASSERT_EQUAL_UINT32(10000, s.maxUs);
    TEST_ASSERT_EQUAL_UINT32(10000, s.lastUs);
    TEST_ASSERT_FALSE(m.lastOverran());
}

void test_loop_monitor_counts_overruns() {
    LoopMonitor m(10000);
    TEST_ASSERT_FALSE(m.record(500));
    TEST_ASSERT_TRUE(m.record(10001));    // one over the budget => overrun
    TEST_ASSERT_TRUE(m.lastOverran());
    TEST_ASSERT_FALSE(m.record(800));     // a fast tick after does not clear the tally
    TEST_ASSERT_TRUE(m.record(25000));
    const LoopStats& s = m.stats();
    TEST_ASSERT_EQUAL_UINT32(4, s.ticks);
    TEST_ASSERT_EQUAL_UINT32(2, s.overruns);
    TEST_ASSERT_EQUAL_UINT32(25000, s.maxUs);  // tracks worst, not last
    TEST_ASSERT_EQUAL_UINT32(25000, s.lastUs);
}

void test_loop_monitor_reset() {
    LoopMonitor m(10000);
    m.record(50000);
    m.reset();
    const LoopStats& s = m.stats();
    TEST_ASSERT_EQUAL_UINT32(0, s.ticks);
    TEST_ASSERT_EQUAL_UINT32(0, s.overruns);
    TEST_ASSERT_EQUAL_UINT32(0, s.maxUs);
    TEST_ASSERT_EQUAL_UINT32(0, s.lastUs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_safe_levels);
    RUN_TEST(test_pulse_open_then_coast);
    RUN_TEST(test_pulse_close_then_coast);
    RUN_TEST(test_never_both_high_on_reversal);
    RUN_TEST(test_independent_timers);
    RUN_TEST(test_diverter_reversal_never_both_high);
    RUN_TEST(test_safe_state_during_open_pulse);
    RUN_TEST(test_diverter_direction);
    RUN_TEST(test_master_and_pump);
    RUN_TEST(test_safe_state);
    RUN_TEST(test_zone_bounds);
    RUN_TEST(test_millis_rollover);

    RUN_TEST(test_rc_begin_idle_safe);
    RUN_TEST(test_rc_full_sequence_completes);
    RUN_TEST(test_rc_diverter_skip_when_unchanged);
    RUN_TEST(test_rc_stop_unwinds);
    RUN_TEST(test_rc_fault_latches_and_clears);
    RUN_TEST(test_rc_fault_from_idle);
    RUN_TEST(test_rc_queue_runs_sequentially);
    RUN_TEST(test_rc_rejects_bad_requests);
    RUN_TEST(test_rc_sw_max_runtime_clamps);
    RUN_TEST(test_rc_watchdog_pre_open_gate);
    RUN_TEST(test_rc_fault_during_prep);
    RUN_TEST(test_rc_stop_during_prep);
    RUN_TEST(test_rc_queue_full_rejected);

    RUN_TEST(test_loop_monitor_within_budget);
    RUN_TEST(test_loop_monitor_counts_overruns);
    RUN_TEST(test_loop_monitor_reset);
    return UNITY_END();
}

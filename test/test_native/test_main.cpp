#include <unity.h>
#include <map>
#include <vector>
#include <utility>
#include <string>
#include "valve_driver.h"
#include "run_controller.h"
#include "loop_monitor.h"
#include "buttons.h"
#include "display.h"
#include "persistence.h"
#include "clock.h"

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
    // begin() must configure every actuator pin as an output — fail-dry depends on
    // the master/pump/bridges being driven, not floating, from boot. Regression
    // guard: RunController::begin() now calls ValveDriver::begin(), not just
    // safeState() (which writes levels but configures nothing).
    TEST_ASSERT_TRUE(g.configured[MASTER] && g.configured[PUMP]);
    TEST_ASSERT_TRUE(g.configured[Z0.in1] && g.configured[Z0.in2]);
    TEST_ASSERT_TRUE(g.configured[Z1.in1] && g.configured[Z1.in2]);
    TEST_ASSERT_TRUE(g.configured[DIV.in1] && g.configured[DIV.in2]);
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

// --- Buttons (firmware spec §11: debounce, edge events, long press) -------------

using tinkle::Buttons;
using tinkle::IButtonInput;

namespace {

struct FakeButtonInput : IButtonInput {
    std::map<uint8_t, bool> down;          // logical pressed state per pin
    std::map<uint8_t, bool> configured;
    void configureInput(uint8_t pin) override { configured[pin] = true; }
    bool isDown(uint8_t pin) override { return down.count(pin) && down[pin]; }
};

// Two synthetic button pins.
constexpr uint8_t B0 = 100;
constexpr uint8_t B1 = 101;

Buttons::Config makeBtnCfg() {
    Buttons::Config c;
    c.pins[0] = B0; c.pins[1] = B1; c.count = 2;
    c.debounceMs = 30; c.longPressMs = 3000;
    return c;
}

} // namespace

// A clean press registers exactly one press edge, only after the debounce window.
void test_button_press_after_debounce() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    b.begin();
    TEST_ASSERT_TRUE(in.configured[B0] && in.configured[B1]);

    b.tick(0);                            // baseline: nothing pressed
    in.down[B0] = true;
    b.tick(10);                           // first observed change — start debounce, no edge yet
    TEST_ASSERT_FALSE(b.pressEdge(0));
    b.tick(30);                           // 30-10=20 ms < 30 ms — still not committed
    TEST_ASSERT_FALSE(b.pressEdge(0));
    b.tick(45);                           // 45-10=35 ms >= 30 ms — press accepted
    TEST_ASSERT_TRUE(b.pressEdge(0));
    TEST_ASSERT_TRUE(b.isDown(0));
    b.tick(50);                           // edge is transient — clears next tick
    TEST_ASSERT_FALSE(b.pressEdge(0));
    TEST_ASSERT_TRUE(b.isDown(0));
}

// A bounce inside the window must not produce a premature edge; one edge once settled.
void test_button_bounce_rejected() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    b.tick(0);
    in.down[B0] = true;  b.tick(10);      // change seen at 10
    in.down[B0] = false; b.tick(20);      // bounced back at 20 — window restarts
    in.down[B0] = true;  b.tick(25);      // changed again at 25 — restart again
    b.tick(40);                           // 40-25=15 < 30 — not yet
    TEST_ASSERT_FALSE(b.pressEdge(0));
    b.tick(60);                           // 60-25=35 >= 30 — single clean press
    TEST_ASSERT_TRUE(b.pressEdge(0));
}

// Release after a short hold: press + release edges, never a long press.
void test_button_short_press_no_long() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    b.tick(0);
    in.down[B0] = true;  b.tick(10); b.tick(45);   // press accepted ~45
    TEST_ASSERT_TRUE(b.pressEdge(0));
    in.down[B0] = false; b.tick(55); b.tick(90);   // release accepted ~90 (held < 3 s)
    TEST_ASSERT_TRUE(b.releaseEdge(0));
    TEST_ASSERT_FALSE(b.isDown(0));
    // No long press anywhere in this cycle.
    TEST_ASSERT_FALSE(b.longPressEdge(0));
}

// A long hold fires exactly one long-press edge once the threshold is crossed.
void test_button_long_press_fires_once() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    b.tick(0);
    in.down[B0] = true; b.tick(10); b.tick(45);    // pressed, stable at 45
    TEST_ASSERT_TRUE(b.pressEdge(0));
    b.tick(2000);                                  // 2000-45 < 3000 — not yet
    TEST_ASSERT_FALSE(b.longPressEdge(0));
    b.tick(3046);                                  // 3046-45 = 3001 >= 3000 — fires
    TEST_ASSERT_TRUE(b.longPressEdge(0));
    b.tick(3100);                                  // does not repeat while held
    TEST_ASSERT_FALSE(b.longPressEdge(0));
    in.down[B0] = false; b.tick(3110); b.tick(3150);
    TEST_ASSERT_TRUE(b.releaseEdge(0));            // release still detected after a long press
}

// Buttons are independent: pressing one leaves the other untouched.
void test_buttons_independent() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    b.tick(0);
    in.down[B1] = true; b.tick(10); b.tick(45);
    TEST_ASSERT_TRUE(b.pressEdge(1));
    TEST_ASSERT_FALSE(b.pressEdge(0));
    TEST_ASSERT_TRUE(b.isDown(1));
    TEST_ASSERT_FALSE(b.isDown(0));
}

// Debounce timing survives a millis() rollover (unsigned subtraction).
void test_button_debounce_rollover() {
    FakeButtonInput in;
    Buttons b(in, makeBtnCfg());
    const uint32_t near = 0xFFFFFFF0u;             // 16 ms before wrap
    b.tick(near);
    in.down[B0] = true;
    b.tick(near + 5);                              // change observed just before wrap
    b.tick(10);                                    // wrapped; (10 - (near+5)) = 21 ms < 30
    TEST_ASSERT_FALSE(b.pressEdge(0));
    b.tick(25);                                    // (25 - (near+5)) = 36 ms >= 30 — accepted
    TEST_ASSERT_TRUE(b.pressEdge(0));
}

// A button held at power-on is adopted as baseline — no press edge, no boot run.
void test_button_held_at_boot_no_edge() {
    FakeButtonInput in;
    in.down[B0] = true;                   // already held when begin() samples
    Buttons b(in, makeBtnCfg());
    b.begin();
    TEST_ASSERT_TRUE(b.isDown(0));        // seen as down...
    b.tick(45); b.tick(90);
    TEST_ASSERT_FALSE(b.pressEdge(0));    // ...but never a fresh press
    // ...and never a phantom long-press once uptime passes the long-press threshold
    // (a real boot's millis() is already well past 3 s when loop() first runs).
    b.tick(5000);
    TEST_ASSERT_FALSE(b.longPressEdge(0));
    // A genuine release then press after boot is a clean edge.
    in.down[B0] = false; b.tick(5100); b.tick(5140);
    TEST_ASSERT_TRUE(b.releaseEdge(0));
    in.down[B0] = true;  b.tick(5150); b.tick(5190);
    TEST_ASSERT_TRUE(b.pressEdge(0));
}

// --- Display (firmware spec §12 + §11 LED rings) --------------------------------

using tinkle::DisplayInputs;
using tinkle::DisplayFrame;
using tinkle::renderDisplay;
using tinkle::LedMode;
using tinkle::RunState;
using tinkle::Fault;

static void assertGlyphs(const DisplayFrame& f, const char* want) {
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_CHAR(want[i], f.glyphs[i]);
}

// Idle, clock synced: HH:MM with the colon steady on.
void test_display_idle_clock() {
    DisplayInputs in;
    in.state = RunState::Idle; in.clockValid = true; in.hours = 8; in.minutes = 34;
    DisplayFrame f = renderDisplay(in, 0);
    assertGlyphs(f, "0834");
    TEST_ASSERT_TRUE(f.colon);
}

// Idle, clock not yet synced: dashes, colon on (§12).
void test_display_idle_unsynced() {
    DisplayInputs in;
    in.state = RunState::Idle; in.clockValid = false;
    DisplayFrame f = renderDisplay(in, 0);
    assertGlyphs(f, "----");
    TEST_ASSERT_TRUE(f.colon);
}

// Running: MM:SS countdown, zero-padded, colon blinks at 1 Hz.
void test_display_running_countdown_and_blink() {
    DisplayInputs in;
    in.state = RunState::Running; in.remainingSec = 125;   // 02:05
    DisplayFrame on  = renderDisplay(in, 0);               // colon-on half
    assertGlyphs(on, "0205");
    TEST_ASSERT_TRUE(on.colon);
    DisplayFrame off = renderDisplay(in, 500);             // colon-off half
    assertGlyphs(off, "0205");
    TEST_ASSERT_FALSE(off.colon);
    DisplayFrame on2 = renderDisplay(in, 1000);            // back on at 1 s
    TEST_ASSERT_TRUE(on2.colon);

    in.remainingSec = 5;   assertGlyphs(renderDisplay(in, 0), "0005");
    in.remainingSec = 599; assertGlyphs(renderDisplay(in, 0), "0959");
}

// Fault: "E" + code, flashing — blank on the off phase.
void test_display_fault_flash() {
    DisplayInputs in;
    in.state = RunState::Fault; in.fault = Fault::NoFlow;
    assertGlyphs(renderDisplay(in, 0),   "E1  ");          // on phase
    assertGlyphs(renderDisplay(in, 500), "    ");          // off phase -> blank
    in.fault = Fault::Watchdog;
    assertGlyphs(renderDisplay(in, 0),   "E3  ");
}

// Transitional states show a steady dash row (§12 implementer's choice).
void test_display_transitional() {
    DisplayInputs in;
    in.state = RunState::PrepDiverter;
    DisplayFrame f = renderDisplay(in, 0);
    assertGlyphs(f, "----");
    TEST_ASSERT_FALSE(f.colon);
}

// Frame equality drives the change-gate (push to TM1637 only when the frame moves).
void test_display_frame_equality() {
    DisplayInputs run;
    run.state = RunState::Running; run.remainingSec = 125;
    DisplayFrame a = renderDisplay(run, 0);      // 02:05, colon on
    DisplayFrame b = renderDisplay(run, 100);    // same second, same colon phase
    TEST_ASSERT_TRUE(a == b);                    // no change -> no push
    DisplayFrame c = renderDisplay(run, 500);    // colon blinked off
    TEST_ASSERT_TRUE(a != c);                    // change -> push
    run.remainingSec = 124;
    DisplayFrame d = renderDisplay(run, 0);      // countdown ticked
    TEST_ASSERT_TRUE(a != d);
}

// LED rings (§11/§12 / DEC-006): solid for the running zone, off otherwise; with no
// dedicated stop ring, a latched fault blinks EVERY zone ring (panel-wide attention),
// overriding the per-zone running/idle level.
void test_display_led_modes() {
    // Not faulted: running zone solid, others off.
    TEST_ASSERT_EQUAL(LedMode::Solid, tinkle::zoneLedMode(1, 1, false));
    TEST_ASSERT_EQUAL(LedMode::Off,   tinkle::zoneLedMode(1, 0, false));
    TEST_ASSERT_EQUAL(LedMode::Off,   tinkle::zoneLedMode(-1, 0, false));  // nothing running
    TEST_ASSERT_EQUAL(LedMode::Solid, tinkle::zoneLedMode(2, 2, false));   // Zone 3 (index 2)
    // Faulted: every ring blinks, regardless of the active zone.
    TEST_ASSERT_EQUAL(LedMode::Blink, tinkle::zoneLedMode(-1, 0, true));
    TEST_ASSERT_EQUAL(LedMode::Blink, tinkle::zoneLedMode(1, 1, true));    // overrides Solid
    TEST_ASSERT_EQUAL(LedMode::Blink, tinkle::zoneLedMode(1, 2, true));
}

// --- Persistence (firmware spec §8 / DEC-008) ------------------------------------
// In-memory IKeyValueStore standing in for NVS. Counts writes per key so write-on-change
// is a tested property, not an assumption.
struct FakeKvStore : tinkle::IKeyValueStore {
    std::map<std::string, uint32_t> u32;
    std::map<std::string, uint8_t>  u8;
    std::map<std::string, int>      writes;

    uint32_t getU32(const char* k, uint32_t def) override {
        auto it = u32.find(k); return it == u32.end() ? def : it->second;
    }
    void putU32(const char* k, uint32_t v) override { u32[k] = v; writes[k]++; }
    uint8_t getU8(const char* k, uint8_t def) override {
        auto it = u8.find(k); return it == u8.end() ? def : it->second;
    }
    void putU8(const char* k, uint8_t v) override { u8[k] = v; writes[k]++; }

    int writesTo(const char* k) const {
        auto it = writes.find(k); return it == writes.end() ? 0 : it->second;
    }
};

using tinkle::Persistence;

void test_persist_defaults_on_empty_nvs() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    // Empty NVS -> every key falls back to its default, no fault.
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, p.zoneDefaultSec(0));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, p.zoneDefaultSec(2));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_SW_MAX_SEC, p.swMaxRuntimeSec());
    TEST_ASSERT_FALSE(p.diverterKnown());
}

void test_persist_schema_stamped_once() {
    FakeKvStore s;
    TEST_ASSERT_EQUAL_UINT32(0, s.getU32("schema_ver", 0));  // absent before first begin
    Persistence p(s, 3);
    p.begin();
    TEST_ASSERT_EQUAL_UINT32(Persistence::SCHEMA_VER, s.getU32("schema_ver", 0));
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("schema_ver"));
    // A reboot against an already-stamped store must not rewrite the version.
    Persistence p2(s, 3);
    p2.begin();
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("schema_ver"));
}

void test_persist_roundtrip_across_reboot() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    p.setZoneDefaultSec(1, 450);
    p.setSwMaxRuntimeSec(900);
    p.setDiverterPosition(true);

    // New instance on the same backing store == reboot: values survive.
    Persistence p2(s, 3);
    p2.begin();
    TEST_ASSERT_EQUAL_UINT32(450, p2.zoneDefaultSec(1));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, p2.zoneDefaultSec(0));  // untouched
    TEST_ASSERT_EQUAL_UINT32(900, p2.swMaxRuntimeSec());
    TEST_ASSERT_TRUE(p2.diverterKnown());
    TEST_ASSERT_TRUE(p2.diverterThrough());
}

void test_persist_write_on_change() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    // Setting a value equal to the current one touches no flash.
    p.setZoneDefaultSec(0, Persistence::DEFAULT_RUN_SEC);
    TEST_ASSERT_EQUAL_INT(0, s.writesTo("z0_dur"));
    // A real change writes exactly once; a redundant repeat does not write again.
    p.setZoneDefaultSec(0, 300);
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("z0_dur"));
    p.setZoneDefaultSec(0, 300);
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("z0_dur"));
}

void test_persist_diverter_tristate() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    TEST_ASSERT_FALSE(p.diverterKnown());        // unknown until first actuation
    p.setDiverterPosition(false);                // AROUND
    TEST_ASSERT_TRUE(p.diverterKnown());
    TEST_ASSERT_FALSE(p.diverterThrough());
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("div_pos"));
    p.setDiverterPosition(false);                // no change -> no write
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("div_pos"));
    p.setDiverterPosition(true);                 // THROUGH
    TEST_ASSERT_TRUE(p.diverterThrough());
    TEST_ASSERT_EQUAL_INT(2, s.writesTo("div_pos"));
}

void test_persist_zone_bounds() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    // Out-of-range zone: read returns the default, write is a silent no-op (no flash).
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, p.zoneDefaultSec(7));
    p.setZoneDefaultSec(7, 123);
    TEST_ASSERT_EQUAL_INT(0, s.writesTo("z7_dur"));
}

// --- Clock (firmware spec §13 / DEC-009) -----------------------------------------
// Fake wall-clock source: availability + the LOCAL epoch it reports are driven by the
// test. Counts polls so the resync throttle is a tested property. The core never sees
// real time — every value is explicit.
struct FakeWallClock : tinkle::IWallClock {
    bool     available = false;
    uint32_t epochVal  = 0;
    int      polls     = 0;
    bool localEpoch(uint32_t& out) override {
        ++polls;
        if (!available) return false;
        out = epochVal;
        return true;
    }
};

using tinkle::Clock;
using tinkle::WallTime;

// A known local epoch: day 1 (1970-01-02, a Friday) at 13:45:30.
//   day 0 = 1970-01-01 = Thursday -> weekday (1+4)%7 = 5 = Friday.
static constexpr uint32_t EPOCH_FRI_1345 = 1u*86400u + 13u*3600u + 45u*60u + 30u;

void test_clock_invalid_until_sync() {
    FakeWallClock src;                       // unavailable
    Clock c(src);
    c.begin(0);
    TEST_ASSERT_FALSE(c.valid());
    TEST_ASSERT_EQUAL_UINT32(0, c.epoch(0));
    WallTime w = c.wall(0);
    TEST_ASSERT_EQUAL_UINT8(0, w.hour);      // zeroed while invalid
    TEST_ASSERT_FALSE(c.minuteRolled(0));    // no per-minute edge until synced
}

void test_clock_syncs_and_derives_fields() {
    FakeWallClock src;
    src.available = true;
    src.epochVal  = EPOCH_FRI_1345;
    Clock c(src);
    c.begin(5000);
    TEST_ASSERT_TRUE(c.valid());
    WallTime w = c.wall(5000);
    TEST_ASSERT_EQUAL_UINT8(13, w.hour);
    TEST_ASSERT_EQUAL_UINT8(45, w.minute);
    TEST_ASSERT_EQUAL_UINT8(30, w.second);
    TEST_ASSERT_EQUAL_UINT8(5,  w.weekday);  // Friday
}

void test_clock_freeruns_between_syncs() {
    FakeWallClock src;
    src.available = true;
    src.epochVal  = EPOCH_FRI_1345;
    Clock c(src);
    c.begin(1000);                           // anchor {EPOCH, ms=1000}
    src.available = false;                   // network drops; only free-run remains
    // 90 s of millis later, wall time has advanced 90 s purely from millis().
    TEST_ASSERT_EQUAL_UINT32(EPOCH_FRI_1345 + 90, c.epoch(1000 + 90000));
    WallTime w = c.wall(1000 + 90000);
    TEST_ASSERT_EQUAL_UINT8(13, w.hour);
    TEST_ASSERT_EQUAL_UINT8(47, w.minute);   // 45:30 + 90 s = 47:00
}

void test_clock_resync_throttled_then_corrects() {
    FakeWallClock src;
    src.available = true;
    src.epochVal  = EPOCH_FRI_1345;
    Clock c(src);
    c.begin(0);                              // synced, anchor at ms=0
    // Source jumps (an NTP correction). Within the resync interval, free-run ignores it.
    src.epochVal = EPOCH_FRI_1345 + 100;
    c.tick(1000);
    TEST_ASSERT_EQUAL_UINT32(EPOCH_FRI_1345 + 1, c.epoch(1000));   // free-run, not the jump
    // At the hourly boundary the clock re-anchors to the source, correcting drift.
    c.tick(Clock::RESYNC_INTERVAL_MS);
    TEST_ASSERT_EQUAL_UINT32(EPOCH_FRI_1345 + 100, c.epoch(Clock::RESYNC_INTERVAL_MS));
}

void test_clock_locks_on_when_source_appears() {
    FakeWallClock src;                       // unavailable at boot
    Clock c(src);
    c.begin(0);
    TEST_ASSERT_FALSE(c.valid());
    int pollsAfterBegin = src.polls;
    // Source comes up (WiFi/NTP lands). A poll before the retry interval is throttled out.
    src.available = true;
    src.epochVal  = EPOCH_FRI_1345;
    c.tick(Clock::ATTEMPT_INTERVAL_MS - 1);
    TEST_ASSERT_EQUAL_INT(pollsAfterBegin, src.polls);   // no extra poll yet
    TEST_ASSERT_FALSE(c.valid());
    // At the retry interval it polls again and locks on.
    c.tick(Clock::ATTEMPT_INTERVAL_MS);
    TEST_ASSERT_TRUE(c.valid());
}

void test_clock_epoch_from_civil() {
    // The packing the ESP32 shim leans on (localtime_r fields -> local epoch). Round-trips
    // the known fixture and a present-day date, and stays consistent with wall()'s inverse.
    using tinkle::epochFromCivil;
    TEST_ASSERT_EQUAL_UINT32(EPOCH_FRI_1345, epochFromCivil(1970, 1, 2, 13, 45, 30));
    TEST_ASSERT_EQUAL_UINT32(0u,             epochFromCivil(1970, 1, 1, 0, 0, 0));
    // 2026-06-06 12:37:06 -> feed it back through a Clock and read the fields out.
    FakeWallClock src;
    src.available = true;
    src.epochVal  = epochFromCivil(2026, 6, 6, 12, 37, 6);
    Clock c(src);
    c.begin(0);
    WallTime w = c.wall(0);
    TEST_ASSERT_EQUAL_UINT8(12, w.hour);
    TEST_ASSERT_EQUAL_UINT8(37, w.minute);
    TEST_ASSERT_EQUAL_UINT8(6,  w.weekday);   // 2026-06-06 is a Saturday
}

void test_clock_minute_rolled_edge() {
    FakeWallClock src;
    src.available = true;
    src.epochVal  = EPOCH_FRI_1345;          // ...:45:30
    Clock c(src);
    c.begin(0);
    TEST_ASSERT_TRUE(c.minuteRolled(0));     // first call after sync fires (eval at startup)
    TEST_ASSERT_FALSE(c.minuteRolled(20000));// same minute, 20 s later -> no edge
    // 30 s more crosses the minute boundary (45:30 -> 46:00).
    TEST_ASSERT_TRUE(c.minuteRolled(35000));
    TEST_ASSERT_FALSE(c.minuteRolled(50000));// 46:something, same minute
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

    RUN_TEST(test_button_press_after_debounce);
    RUN_TEST(test_button_bounce_rejected);
    RUN_TEST(test_button_short_press_no_long);
    RUN_TEST(test_button_long_press_fires_once);
    RUN_TEST(test_buttons_independent);
    RUN_TEST(test_button_debounce_rollover);
    RUN_TEST(test_button_held_at_boot_no_edge);

    RUN_TEST(test_display_idle_clock);
    RUN_TEST(test_display_idle_unsynced);
    RUN_TEST(test_display_running_countdown_and_blink);
    RUN_TEST(test_display_fault_flash);
    RUN_TEST(test_display_transitional);
    RUN_TEST(test_display_frame_equality);
    RUN_TEST(test_display_led_modes);

    RUN_TEST(test_persist_defaults_on_empty_nvs);
    RUN_TEST(test_persist_schema_stamped_once);
    RUN_TEST(test_persist_roundtrip_across_reboot);
    RUN_TEST(test_persist_write_on_change);
    RUN_TEST(test_persist_diverter_tristate);
    RUN_TEST(test_persist_zone_bounds);

    RUN_TEST(test_clock_invalid_until_sync);
    RUN_TEST(test_clock_syncs_and_derives_fields);
    RUN_TEST(test_clock_freeruns_between_syncs);
    RUN_TEST(test_clock_resync_throttled_then_corrects);
    RUN_TEST(test_clock_locks_on_when_source_appears);
    RUN_TEST(test_clock_epoch_from_civil);
    RUN_TEST(test_clock_minute_rolled_edge);
    return UNITY_END();
}

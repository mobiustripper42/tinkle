#include <unity.h>
#include <map>
#include <vector>
#include <utility>
#include <string>
#include "valve_driver.h"
#include "run_controller.h"
#include "loop_monitor.h"
#include "persistence.h"
#include "clock.h"
#include "scheduler.h"
#include "wifi_link_monitor.h"
#include "flow_monitor.h"
#include "flow_fault_detector.h"
#include "calibration_controller.h"
#include "watchdog_trip.h"
#include "watchdog.h"
#include "fault_manager.h"
#include "valve_rest_monitor.h"
#include "api.h"

// Native unit tests for ValveDriver (firmware spec §5, v1.4). Fake GPIO + an explicit
// clock value (no millis()): the host-testable tier per CLAUDE.md.
//
// v1.4 (DEC-011/012/013): every valve is a single low-side FET — no H-bridges, no
// never-both-high invariant. The fake just records writes (level, ordered log) so the
// tests can assert FET levels and write ordering directly.

using tinkle::IGpio;
using tinkle::ValveConfig;
using tinkle::ValveDriver;

namespace {

struct FakeGpio : IGpio {
    std::map<uint8_t, bool> level;        // current commanded level per pin
    std::map<uint8_t, bool> configured;   // pins set as outputs
    std::vector<std::pair<uint8_t, bool>> log;  // ordered write history

    void configureOutput(uint8_t pin) override { configured[pin] = true; }

    void write(uint8_t pin, bool high) override {
        level[pin] = high;
        log.push_back({pin, high});
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

// Synthetic pin map — values are arbitrary, just distinct. One FET per valve.
constexpr uint8_t ZF0       = 2;   // zone 0 FET
constexpr uint8_t ZF1       = 3;   // zone 1 FET
constexpr uint8_t DIV_CLEAN = 6;   // NO bypass leg  (low = open = plain)
constexpr uint8_t DIV_FERT  = 7;   // NC Dosatron leg (high = open = fert)
constexpr uint8_t PUMP      = 9;

// Synthetic travel windows kept short so the cooperative-loop tests run fast — the
// real ZONE_TRAVEL_MS / DIVERTER_TRAVEL_MS (10 s, §15) live in ValveConfig's defaults.
ValveConfig makeConfig() {
    ValveConfig c;
    c.zoneFet[0] = ZF0;
    c.zoneFet[1] = ZF1;
    c.zoneCount = 2;
    c.divCleanFet = DIV_CLEAN;
    c.divFertFet  = DIV_FERT;
    c.pumpRelay = PUMP;
    c.zoneTravelMs = 75;
    c.diverterTravelMs = 6000;
    return c;
}

FakeGpio g;
ValveConfig cfg;

} // namespace

void setUp() {
    g = FakeGpio{};
    cfg = makeConfig();
}
void tearDown() {}

// begin() forces the safe/rest state: every valve FET off (NC zones closed, NC fert
// closed, NO bypass open => plain), pump off.
static void test_begin_safe_levels() {
    ValveDriver vd(g, cfg);
    vd.begin();
    TEST_ASSERT_FALSE(g.level[ZF0]); TEST_ASSERT_FALSE(g.level[ZF1]);
    TEST_ASSERT_FALSE(g.level[DIV_CLEAN]); TEST_ASSERT_FALSE(g.level[DIV_FERT]);
    TEST_ASSERT_FALSE(g.level[PUMP]);
    TEST_ASSERT_TRUE(g.configured[ZF0] && g.configured[ZF1]);
    TEST_ASSERT_TRUE(g.configured[DIV_CLEAN] && g.configured[DIV_FERT] && g.configured[PUMP]);
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(vd.diverterFert());           // rest = plain
}

// openZone energizes the zone FET; the valve HOLDS open (FET stays high) while
// energized, busy until the travel window elapses — no coast-back.
static void test_open_zone_holds() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.openZone(0, 1000);
    TEST_ASSERT_TRUE(g.level[ZF0]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    TEST_ASSERT_TRUE(vd.busy());

    vd.tick(1000 + 74);                 // one ms short of travel — still traveling
    TEST_ASSERT_TRUE(g.level[ZF0]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));

    vd.tick(1000 + 75);                 // travel done — busy clears, FET STAYS high (held open)
    TEST_ASSERT_TRUE(g.level[ZF0]);
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(vd.busy());
}

// closeZone de-energizes the FET (cap auto-return drives the valve closed); busy for
// the travel window, FET held low.
static void test_close_zone_deenergizes() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.openZone(1, 0);
    vd.tick(75);                        // open travel done, FET held high
    TEST_ASSERT_TRUE(g.level[ZF1]);
    vd.closeZone(1, 500);
    TEST_ASSERT_FALSE(g.level[ZF1]);
    TEST_ASSERT_TRUE(vd.zoneBusy(1));
    vd.tick(500 + 75);
    TEST_ASSERT_FALSE(g.level[ZF1]);
    TEST_ASSERT_FALSE(vd.zoneBusy(1));
}

// Independent per-actuator timers: a 6 s diverter travel must not hold up a zone's
// travel, and the zone finishing must not cut the diverter short.
static void test_independent_timers() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.openZone(0, 0);                  // zone travel 0..75
    vd.setDiverter(true, 10);          // fert, 10..6010
    TEST_ASSERT_TRUE(g.level[DIV_FERT]); TEST_ASSERT_TRUE(g.level[DIV_CLEAN]);
    TEST_ASSERT_TRUE(vd.diverterFert());

    vd.tick(80);                       // zone travel done; diverter still traveling
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_TRUE(g.level[ZF0]);    // zone held open
    TEST_ASSERT_TRUE(vd.diverterBusy());

    vd.tick(10 + 6000);                // travel window elapsed
    TEST_ASSERT_FALSE(vd.diverterBusy());
    TEST_ASSERT_TRUE(g.level[DIV_FERT]);   // legs HELD at fert (high) — no coast-back
    TEST_ASSERT_TRUE(g.level[DIV_CLEAN]);
    TEST_ASSERT_FALSE(vd.busy());
}

// safeState landing while a zone is mid-open-travel (the §14 fault case): the FET
// drops to LOW immediately (cap-return closes the valve), busy clears.
static void test_safe_state_during_open() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.openZone(0, 0);                  // zone 0 mid-open
    TEST_ASSERT_TRUE(g.level[ZF0]);
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    vd.safeState(30);                  // fault unwind before the travel completes
    TEST_ASSERT_FALSE(g.level[ZF0]);
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_FALSE(vd.pumpIsOn());
}

// Diverter leg mapping (DEC-013): fertigate => both legs HIGH (fert opens, bypass
// closes); plain => both LOW (bypass open, fert closed). Legs hold their level.
static void test_diverter_legs() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.setDiverter(true, 0);           // fertigate
    TEST_ASSERT_TRUE(g.level[DIV_FERT]);
    TEST_ASSERT_TRUE(g.level[DIV_CLEAN]);
    TEST_ASSERT_TRUE(vd.diverterFert());
    vd.setDiverter(false, 100);        // back to plain
    TEST_ASSERT_FALSE(g.level[DIV_FERT]);
    TEST_ASSERT_FALSE(g.level[DIV_CLEAN]);
    TEST_ASSERT_FALSE(vd.diverterFert());
}

// Pump relay is an immediate level set with a matching state getter (no master in v1.4).
static void test_pump() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.pumpOn();  TEST_ASSERT_TRUE(g.level[PUMP]);  TEST_ASSERT_TRUE(vd.pumpIsOn());
    vd.pumpOff(); TEST_ASSERT_FALSE(g.level[PUMP]); TEST_ASSERT_FALSE(vd.pumpIsOn());
}

// safeState: pump off, every valve FET de-energized (zones cap-close, diverter to
// plain) — and the pump is commanded off BEFORE the valves (§14 unwind order).
static void test_safe_state() {
    ValveDriver vd(g, cfg);
    vd.begin();
    vd.openZone(0, 0); vd.openZone(1, 0);
    vd.setDiverter(true, 0);
    vd.pumpOn();

    vd.safeState(2000);

    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(g.level[ZF0]); TEST_ASSERT_FALSE(g.level[ZF1]);   // zones de-energized
    TEST_ASSERT_FALSE(g.level[DIV_CLEAN]); TEST_ASSERT_FALSE(g.level[DIV_FERT]);
    TEST_ASSERT_FALSE(vd.diverterFert());           // returned to plain (§5)
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    // pump-off precedes the zone de-energize in the write log
    TEST_ASSERT_TRUE(g.lastIndexOf(PUMP) < g.lastIndexOf(ZF0));
}

// Out-of-range zone is a no-op (no writes, no crash).
static void test_zone_bounds() {
    ValveDriver vd(g, cfg);
    vd.begin();
    int before = (int)g.log.size();
    vd.openZone(7, 0);
    vd.closeZone(7, 0);
    TEST_ASSERT_EQUAL_INT(before, (int)g.log.size());
    TEST_ASSERT_FALSE(vd.zoneBusy(7));
}

// A millis() rollover mid-travel must still release the busy flag on time (unsigned math).
static void test_millis_rollover() {
    ValveDriver vd(g, cfg);
    vd.begin();
    const uint32_t start = 0xFFFFFFF0;   // 16 ms before wrap
    vd.openZone(0, start);
    vd.tick(start + 40);                 // 40 ms elapsed (wrapped past 0) — still < 75
    TEST_ASSERT_TRUE(vd.zoneBusy(0));
    vd.tick(start + 75);                 // 75 ms elapsed across the wrap — travel done
    TEST_ASSERT_FALSE(vd.zoneBusy(0));
    TEST_ASSERT_TRUE(g.level[ZF0]);      // held open
}

// ----------------------------------------------------------------------------
// RunController (firmware spec §4). The same FakeGpio (so FET levels are observable
// across whole runs), a real ValveDriver, and an explicit clock. Faults are injected
// via raiseFault(), matching how FlowMonitor/Watchdog will notify it later.
// ----------------------------------------------------------------------------

using tinkle::Fault;
using tinkle::RunConfig;
using tinkle::RunController;
using tinkle::RunRequest;
using tinkle::RunState;
using tinkle::RunResult;
using tinkle::RunEntry;
using tinkle::RunLog;

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
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_EQUAL_INT(-1, rc.activeZone());
    // begin() must configure every actuator pin as an output — fail-dry depends on
    // the pump + valve FETs being driven, not floating, from boot. Regression guard:
    // RunController::begin() calls ValveDriver::begin(), not just safeState() (which
    // writes levels but configures nothing).
    TEST_ASSERT_TRUE(g.configured[PUMP]);
    TEST_ASSERT_TRUE(g.configured[ZF0] && g.configured[ZF1]);
    TEST_ASSERT_TRUE(g.configured[DIV_CLEAN] && g.configured[DIV_FERT]);
}

// Full happy-path run: PREP -> OPEN_ZONE -> START_PUMP -> RUNNING for the duration
// -> STOP_PUMP -> CLOSE_ZONE -> SETTLE -> IDLE, pump off at the end, logged Completed.
static void test_rc_full_sequence_completes() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 2; req.fertigate = false;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));

    // Reach RUNNING (plain run: the diverter is already at rest=plain, so no travel).
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_TRUE(vd.pumpIsOn());
    TEST_ASSERT_TRUE(g.level[ZF0]);                  // zone valve energized open
    TEST_ASSERT_EQUAL_INT(0, rc.activeZone());
    TEST_ASSERT_TRUE(rc.remainingSec(now) >= 1 && rc.remainingSec(now) <= 2);

    // Run out the 2 s duration + settle -> IDLE.
    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(g.level[ZF0]);                 // zone de-energized closed
    TEST_ASSERT_EQUAL(RunResult::Completed, rc.lastRun().result);
    TEST_ASSERT_EQUAL_UINT8(0, rc.lastRun().zoneIndex);
}

// The diverter skip-travel guard (§6) survives DEC-021 inside a QUEUE: with a run still
// queued, SETTLE leaves the legs as-is, so a chained same-state run reaches RUNNING
// without re-driving them. (Across SEPARATE runs the diverter returns to plain at SETTLE
// — see test_rc_diverter_returns_plain_at_settle — so back-to-back fert via two requestRun
// calls DOES re-travel; the queue is the only surviving consecutive-skip path.)
static void test_rc_diverter_skip_when_unchanged() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1; req.fertigate = true;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));        // first fert run
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));        // second fert run queued behind it
    TEST_ASSERT_EQUAL_UINT8(1, rc.queueDepth());

    // First fert run reaches RUNNING — legs travel to fert exactly once.
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(vd.diverterFert());
    int divWritesBefore = g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN);

    // Hand off to the queued fert run. The first run's SETTLE must NOT return to plain
    // while a run is queued (qCount_ > 0), so the chained run skips travel: no new writes.
    now = pump(rc, now, 5, 8000,
               [&]{ return rc.state() == RunState::Running && rc.queueDepth() == 0; });
    TEST_ASSERT_TRUE(vd.diverterFert());
    TEST_ASSERT_EQUAL_INT(divWritesBefore, g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN));
}

// DEC-021: on a clean run end with NOTHING queued, the diverter returns to its plain
// rest state at SETTLE — both legs de-energized — restoring the documented rest state
// (§5/§14, DEC-011/013) instead of resting fert-open between runs for up to ~24h.
static void test_rc_diverter_returns_plain_at_settle() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1; req.fertigate = true;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(vd.diverterFert());            // fert leg open mid-run
    TEST_ASSERT_TRUE(g.level[DIV_FERT]);

    now = pump(rc, now, 5, 4000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.diverterFert());           // returned to plain
    TEST_ASSERT_FALSE(g.level[DIV_FERT]);           // fert leg de-energized (closed)
    TEST_ASSERT_FALSE(g.level[DIV_CLEAN]);          // bypass de-energized (open = plain)
}

// DEC-021 sequencing seam: the SETTLE return-to-plain leaves the diverter mid-travel
// (divTimer busy ~diverterTravelMs) while the controller advances to IDLE on the shorter
// settleMs. A run requested during that window must wait the residual return-travel out in
// PREP — not open the zone while the legs are still relaxing.
static void test_rc_run_waits_residual_diverter_return() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);

    // Fert run to completion -> IDLE. SETTLE fired setDiverter(false): divTimer busy ~6 s.
    RunRequest fert; fert.zoneIndex = 0; fert.durationSec = 1; fert.fertigate = true;
    rc.requestRun(fert, 0);
    uint32_t now = pump(rc, 0, 5, 12000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_TRUE(vd.diverterBusy());            // return-travel still in flight at IDLE

    // A plain run requested now holds in PREP behind the residual return-travel. It needs
    // no leg change (already returning to plain), so PrepDiverter's !diverterBusy() gate —
    // not a fresh setDiverter — is what makes it wait.
    RunRequest plain; plain.zoneIndex = 0; plain.durationSec = 1; plain.fertigate = false;
    rc.requestRun(plain, now);
    rc.tick(now);
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());

    // Partway through the ~5.9 s residual: still gated in PREP, zone never opened.
    now = pump(rc, now, 5, 3000, [&]{ return rc.state() != RunState::PrepDiverter; });
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());
    TEST_ASSERT_FALSE(g.level[ZF0]);

    // Once the return-travel clears, the run proceeds to RUNNING, legs still plain.
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_FALSE(vd.diverterFert());
    (void)now;
}

// A plain run's SETTLE must not touch the diverter legs (the diverterFert() guard): the
// diverter is already at plain rest, so there is nothing to return and no spurious ~10s
// travel. Regression guard on the DEC-021 SETTLE return — a plain run writes the legs zero times.
static void test_rc_plain_run_settle_no_diverter_travel() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    int divWritesBefore = g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_FALSE(vd.diverterFert());
    TEST_ASSERT_EQUAL_INT(divWritesBefore, g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN));
    (void)now;
}

// v1.4 boot state is known to be plain (both legs de-energized), so a first PLAIN run
// skips the diverter travel entirely — it never commands the legs and reaches RUNNING
// far inside the diverter-travel window. (DEC-013: no NVS cache, the rest state is by
// construction.)
static void test_rc_first_plain_run_skips_diverter() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    int divWritesBefore = g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 2000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_TRUE(now < 6000);                    // diverter travel skipped, not waited out
    TEST_ASSERT_EQUAL_INT(divWritesBefore, g.writesTo(DIV_FERT) + g.writesTo(DIV_CLEAN));
    TEST_ASSERT_FALSE(vd.diverterFert());
}

// Graceful stop() mid-run unwinds to IDLE with the pump off; logged Stopped.
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
    TEST_ASSERT_EQUAL(RunResult::Stopped, rc.lastRun().result);
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
    TEST_ASSERT_EQUAL(RunResult::Faulted, rc.lastRun().result);

    // Refused while latched.
    TEST_ASSERT_FALSE(rc.requestRun(req, now));

    // Clear, then a fresh run reaches RUNNING.
    TEST_ASSERT_TRUE(rc.clearFault());
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    TEST_ASSERT_TRUE(rc.requestRun(req, now));
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
}

// Unexpected flow during IDLE faults straight to safe state (§14 idle case).
static void test_rc_fault_from_idle() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    rc.raiseFault(Fault::UnexpectedFlow, 500);
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow, rc.activeFault());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
}

// #138: a fault raised while IDLE has no run in flight — current_/pending* still hold the
// PREVIOUS run, so logging would push a phantom Faulted duplicate of it. The fault still
// latches; it just adds no RunLog row.
static void test_rc_idle_fault_logs_no_phantom() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest a; a.zoneIndex = 0; a.durationSec = 1; a.fertigate = false;
    TEST_ASSERT_TRUE(rc.requestRun(a, 0));
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Idle; });
    TEST_ASSERT_EQUAL(RunState::Idle, rc.state());
    const uint8_t before = rc.runLog().count();
    TEST_ASSERT_EQUAL(RunResult::Completed, rc.lastRun().result);   // the one real run

    rc.raiseFault(Fault::UnexpectedFlow, now + 100);                // idle-time E2
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());                 // still latches
    TEST_ASSERT_EQUAL_UINT8(before, rc.runLog().count());          // but no phantom row
    TEST_ASSERT_EQUAL(RunResult::Completed, rc.lastRun().result);  // last row untouched
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

// §4 step 2 (DEC-023, wait-not-kill): a trip asserted at run start HOLDS the run in
// PrepDiverter — nothing energizes — and the run proceeds once the line releases.
static void test_rc_watchdog_pre_open_gate_holds_then_proceeds() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    rc.setWatchdogTripped(true);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 2000, [&]{ return false; });   // 2 s of held ticks
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());       // holding, not dead
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_FALSE(g.level[ZF0]);             // zone valve never energized
    TEST_ASSERT_FALSE(vd.pumpIsOn());            // pump never started
    rc.setWatchdogTripped(false);                // lockout self-released
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());            // the run happened
    TEST_ASSERT_TRUE(vd.pumpIsOn());
}

// DEC-023: a line stuck asserted past wdWaitMs skips THAT run only — logged Faulted,
// nothing energized — and the next queued run gets its own chance at the gate.
static void test_rc_watchdog_stuck_line_skips_run_keeps_queue() {
    ValveDriver vd(g, cfg);
    RunConfig rcfg = makeRunCfg();
    rcfg.wdWaitMs = 500;                          // short for tests; real seed 60 s (§15)
    RunController rc(vd, rcfg);
    rc.begin(0);
    rc.setWatchdogTripped(true);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5;
    RunRequest req2; req2.zoneIndex = 1; req2.durationSec = 5;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));
    TEST_ASSERT_TRUE(rc.requestRun(req2, 0));    // queued behind the held run
    uint32_t now = pump(rc, 0, 5, 4000,
                        [&]{ return rc.lastRun().result == RunResult::Faulted; });
    TEST_ASSERT_EQUAL(RunResult::Faulted, rc.lastRun().result);   // run 1 skipped + logged
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Fault::Watchdog, rc.lastRun().faultCode);
    TEST_ASSERT_FALSE(rc.isFaulted());                            // never latched
    TEST_ASSERT_FALSE(g.level[ZF0]);                              // zone 0 never opened
    // Run 2 is now holding at its own gate; release the line and it waters.
    rc.setWatchdogTripped(false);
    now = pump(rc, now, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_EQUAL_INT(1, rc.activeZone());
}

// DEC-023: a confirmed trip mid-run ABORTS that run (normal unwind, logged Faulted)
// and PRESERVES the queue — one trip must never cost the rest of the schedule.
// raiseFault(Watchdog) is structurally refused, like ValveRest.
static void test_rc_watchdog_abort_preserves_queue_never_latches() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5;
    RunRequest req2; req2.zoneIndex = 1; req2.durationSec = 5;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));
    TEST_ASSERT_TRUE(rc.requestRun(req2, 0));
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });

    TEST_ASSERT_TRUE(rc.abortRun(Fault::Watchdog, now));       // trip confirmed mid-run
    TEST_ASSERT_FALSE(rc.abortRun(Fault::Watchdog, now));      // once per run
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_EQUAL_UINT8(1, rc.queueDepth());               // queue intact
    TEST_ASSERT_FALSE(vd.pumpIsOn());                          // unwinding

    // The abort unwinds through Settle (logged Faulted) and run 2 starts.
    now = pump(rc, now, 5, 30000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_EQUAL_INT(1, rc.activeZone());
    // The ring holds run 1's Faulted entry beneath run 2's in-flight state.
    bool sawFaulted = false;
    for (uint8_t i = 0; i < rc.runLog().count(); ++i)
        if (rc.runLog().at(i).result == RunResult::Faulted &&
            rc.runLog().at(i).faultCode == (uint8_t)Fault::Watchdog) sawFaulted = true;
    TEST_ASSERT_TRUE(sawFaulted);

    // Structural refusal: the old latching path is gone.
    rc.raiseFault(Fault::Watchdog, now);
    TEST_ASSERT_FALSE(rc.isFaulted());
}

// The REAL call path, wired the way main.cpp wires it: Watchdog::tick() produces
// the verdict, tripConfirmed() feeds the gate, the verdict routes to abortRun().
// A run parked in PrepDiverter with a CONFIRMED trip must HOLD (abortRun refuses
// there — the wait-gate owns that state), then proceed on release. Locks the
// review finding where the verdict preempted the hold.
static void test_rc_watchdog_verdict_wiring_holds_in_prep() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    tinkle::Watchdog wd(g, 21, tinkle::Watchdog::Config{});   // pin 21 = the synthetic
    wd.begin(0);                                  // HB pin; tripConfirmMs = 100 default

    // The field sequence: the line confirms while IDLE (no verdict there), THEN a
    // run starts — it must park at the gate, not die.
    uint32_t now = 0;
    for (; now < 500; now += 50) {
        rc.tick(now);
        wd.tick(rc.state(), true, now);
        rc.setWatchdogTripped(wd.tripConfirmed());
    }
    TEST_ASSERT_TRUE(wd.tripConfirmed());

    RunRequest req; req.zoneIndex = 0; req.durationSec = 5;
    TEST_ASSERT_TRUE(rc.requestRun(req, now));    // enters PrepDiverter

    // 2 s of ticks with the trip line still held — main.cpp's exact sequence.
    // PrepDiverter is an active state, so the verdict fires every tick; abortRun
    // must refuse it and leave the hold in charge.
    for (uint32_t end = now + 2000; now < end; now += 50) {
        rc.tick(now);
        const Fault v = wd.tick(rc.state(), true, now);
        rc.setWatchdogTripped(wd.tripConfirmed());
        if (v != Fault::None) rc.abortRun(v, now);
    }
    TEST_ASSERT_EQUAL(RunState::PrepDiverter, rc.state());   // held, not skipped
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)rc.runLog().count());// nothing logged Faulted

    // Line releases — the run proceeds to water.
    for (; now < 10000 && rc.state() != RunState::Running; now += 50) {
        rc.tick(now);
        const Fault v = wd.tick(rc.state(), false, now);
        rc.setWatchdogTripped(wd.tripConfirmed());
        if (v != Fault::None) rc.abortRun(v, now);
    }
    TEST_ASSERT_EQUAL(RunState::Running, rc.state());
    TEST_ASSERT_TRUE(vd.pumpIsOn());
}

// An operator stop landing during an in-flight watchdog abort wins the log entry:
// the run records Stopped, not Faulted (review finding — DEC-023).
static void test_rc_stop_wins_over_abort_log() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 0; req.durationSec = 5;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(rc.abortRun(Fault::Watchdog, now));
    rc.stop(now);                                            // operator stop mid-unwind
    now = pump(rc, now, 5, 8000, [&]{ return rc.isIdle(); });
    TEST_ASSERT_EQUAL(RunResult::Stopped, rc.lastRun().result);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Fault::None, rc.lastRun().faultCode);
}

// A fault landing while the diverter is mid-travel (before the zone ever energizes)
// still slams the safe state and never starts the pump.
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
    TEST_ASSERT_FALSE(g.level[ZF0]);             // zone never energized during prep
    TEST_ASSERT_FALSE(vd.pumpIsOn());
}

// stop() during PREP_DIVERTER unwinds to IDLE while the diverter coasts in the
// background — pump + valves safe.
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

// --- Persistence (firmware spec §8 / DEC-008) ------------------------------------
// In-memory IKeyValueStore standing in for NVS. Counts writes per key so write-on-change
// is a tested property, not an assumption.
struct FakeKvStore : tinkle::IKeyValueStore {
    std::map<std::string, uint32_t> u32;
    std::map<std::string, uint8_t>  u8;
    std::map<std::string, float>    f32;
    std::map<std::string, int>      writes;

    uint32_t getU32(const char* k, uint32_t def) override {
        auto it = u32.find(k); return it == u32.end() ? def : it->second;
    }
    void putU32(const char* k, uint32_t v) override { u32[k] = v; writes[k]++; }
    uint8_t getU8(const char* k, uint8_t def) override {
        auto it = u8.find(k); return it == u8.end() ? def : it->second;
    }
    void putU8(const char* k, uint8_t v) override { u8[k] = v; writes[k]++; }
    float getFloat(const char* k, float def) override {
        auto it = f32.find(k); return it == f32.end() ? def : it->second;
    }
    void putFloat(const char* k, float v) override { f32[k] = v; writes[k]++; }

    std::map<std::string, std::string>          str;
    std::map<std::string, std::vector<uint8_t>> bytes;

    bool getStr(const char* k, char* out, uint16_t cap) override {
        auto it = str.find(k);
        if (it == str.end()) { if (cap) out[0] = '\0'; return false; }
        uint16_t i = 0;
        for (; i + 1 < cap && i < it->second.size(); ++i) out[i] = it->second[i];
        out[i] = '\0';
        return true;
    }
    void putStr(const char* k, const char* v) override { str[k] = v; writes[k]++; }
    uint16_t getBytes(const char* k, void* out, uint16_t cap) override {
        auto it = bytes.find(k);
        if (it == bytes.end()) return 0;
        uint16_t n = (uint16_t)it->second.size();
        if (n > cap) n = cap;
        for (uint16_t i = 0; i < n; ++i) static_cast<uint8_t*>(out)[i] = it->second[i];
        return n;
    }
    void putBytes(const char* k, const void* data, uint16_t len) override {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bytes[k].assign(p, p + len);
        writes[k]++;
    }

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

    // New instance on the same backing store == reboot: values survive.
    Persistence p2(s, 3);
    p2.begin();
    TEST_ASSERT_EQUAL_UINT32(450, p2.zoneDefaultSec(1));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, p2.zoneDefaultSec(0));  // untouched
    TEST_ASSERT_EQUAL_UINT32(900, p2.swMaxRuntimeSec());
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
    // Leap-day boundary — the civil-arithmetic edge most likely to be wrong. 2024 is a leap
    // year, so 2024-02-29 is a real date; the day after is 2024-03-01, exactly 86400 s later.
    TEST_ASSERT_EQUAL_UINT32(epochFromCivil(2024, 2, 29, 0, 0, 0) + 86400u,
                             epochFromCivil(2024, 3, 1, 0, 0, 0));
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

// --- Scheduler (firmware spec §13 + §6 / DEC-009) --------------------------------
// Fake run sink: records every accepted request and can simulate a full-queue rejection,
// so the scheduler's eval / fert / overlap logic is tested in isolation from the run state
// machine.
struct FakeRunSink : tinkle::IRunSink {
    std::vector<tinkle::RunRequest> reqs;
    bool accept = true;
    bool requestRun(const tinkle::RunRequest& r, uint32_t) override {
        if (!accept) return false;
        reqs.push_back(r);
        return true;
    }
};

using tinkle::Scheduler;
using tinkle::ScheduleEntry;
using tinkle::FertOverride;
using tinkle::DistributedConfig;
using tinkle::DistributedPlan;
using tinkle::computeDistributedPlan;
using tinkle::packDistributedConfig;
using tinkle::unpackDistributedConfig;
using tinkle::DIST_CONFIG_BYTES;
using tinkle::DIST_MAX_RUNS;
using tinkle::epochFromCivil;

// A clock pinned to a given local epoch (anchored at ms=0, so epoch advances with nowMs).
// Returned by value into a holder the tests keep alive.
static ScheduleEntry mkEntry(uint8_t zone, uint8_t hh, uint8_t mm, uint8_t daysMask,
                             uint16_t dur = 300, FertOverride f = FertOverride::Auto) {
    ScheduleEntry e;
    e.zoneIndex = zone; e.hour = hh; e.minute = mm; e.daysMask = daysMask;
    e.durationSec = dur; e.fertOverride = f; e.enabled = true;
    return e;
}

// 2026-06-08 is a Monday (2026-06-06 was Saturday). Mon weekday = 1, bit 0x02.
static constexpr uint8_t MON_BIT = 1u << 1;
static constexpr uint8_t DAILY   = 0x7F;

void test_sched_fires_due_entry() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);   // Monday 06:00
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(2, 6, 0, MON_BIT, 420));
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(1, (int)sink.reqs.size());
    TEST_ASSERT_EQUAL_UINT8(2, sink.reqs[0].zoneIndex);
    TEST_ASSERT_EQUAL_UINT32(420, sink.reqs[0].durationSec);
}

void test_sched_skips_wrong_minute_day_disabled() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 1, 0);   // Monday 06:01
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, MON_BIT));                     // wrong minute (06:00 vs 06:01)
    s.add(mkEntry(1, 6, 1, 1u << 2));                     // right time, Tuesday-only -> wrong day
    ScheduleEntry off = mkEntry(2, 6, 1, MON_BIT); off.enabled = false;
    s.add(off);                                           // right time/day but disabled
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(0, (int)sink.reqs.size());
}

void test_sched_invalid_clock_no_fire() {
    FakeWallClock src;                                    // unavailable -> clock invalid
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, DAILY));
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(0, (int)sink.reqs.size());
}

void test_sched_idempotent_within_minute() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, MON_BIT));
    s.tick(0);
    s.tick(100);        // same minute, 0.1 s later
    s.tick(59000);      // still 06:00
    TEST_ASSERT_EQUAL_INT(1, (int)sink.reqs.size());   // evaluated once for the minute
    // Advance into 06:01, then a DEC-009 sub-second backward nudge to 06:00:59. Forward-only
    // eval must NOT re-fire 06:00 (the previous minute), which a == guard would have missed.
    s.add(mkEntry(1, 6, 1, MON_BIT));
    s.tick(60000);      // 06:01 -> zone 1 fires
    TEST_ASSERT_EQUAL_INT(2, (int)sink.reqs.size());
    s.tick(59900);      // nudged back to 06:00:59 -> must be a no-op
    TEST_ASSERT_EQUAL_INT(2, (int)sink.reqs.size());
}

void test_sched_fert_first_run_of_day() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);   // Monday 06:00
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, DAILY));                      // first auto run of the day
    s.add(mkEntry(1, 6, 5, DAILY));                      // second auto run, +5 min
    s.tick(0);                                           // 06:00 -> zone 0
    s.tick(5u * 60u * 1000u);                            // 06:05 -> zone 1
    TEST_ASSERT_EQUAL_INT(2, (int)sink.reqs.size());
    TEST_ASSERT_TRUE(sink.reqs[0].fertigate);            // first auto run fertigates
    TEST_ASSERT_FALSE(sink.reqs[1].fertigate);           // the rest don't
    // Next calendar day: the auto-fert slot resets.
    s.tick(86400u * 1000u);                              // Tuesday 06:00 -> zone 0 again
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());
    TEST_ASSERT_TRUE(sink.reqs[2].fertigate);
}

void test_sched_fert_overrides() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, DAILY, 300, FertOverride::Off));   // first run, but forced off
    s.add(mkEntry(1, 6, 0, DAILY, 300, FertOverride::On));    // forced on
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(2, (int)sink.reqs.size());
    TEST_ASSERT_FALSE(sink.reqs[0].fertigate);   // Off forces no fert despite being first
    TEST_ASSERT_TRUE(sink.reqs[1].fertigate);    // On forces fert
}

void test_sched_dropped_on_full_queue() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink; sink.accept = false;       // sink refuses every run (queue full / fault)
    Scheduler s(sink, c);
    s.add(mkEntry(0, 6, 0, DAILY));
    s.add(mkEntry(1, 6, 0, DAILY));
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(0, (int)sink.reqs.size());
    TEST_ASSERT_EQUAL_UINT32(2, s.dropped());
}

void test_sched_eval_now_on_edit() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c);
    s.tick(0);                                   // empty schedule -> nothing
    TEST_ASSERT_EQUAL_INT(0, (int)sink.reqs.size());
    s.add(mkEntry(0, 6, 0, MON_BIT));            // edit mid-minute
    s.evalNow(0);                                // §13 "checks ... on edit" -> fires immediately
    TEST_ASSERT_EQUAL_INT(1, (int)sink.reqs.size());
}

void test_sched_add_capacity() {
    FakeWallClock src;
    Clock c(src);
    FakeRunSink sink;
    Scheduler s(sink, c);
    for (uint8_t i = 0; i < Scheduler::MAX_ENTRIES; ++i)
        TEST_ASSERT_TRUE(s.add(mkEntry(0, 6, 0, DAILY)));
    TEST_ASSERT_FALSE(s.add(mkEntry(0, 6, 0, DAILY)));        // full -> rejected
    TEST_ASSERT_EQUAL_UINT8(Scheduler::MAX_ENTRIES, s.count());
    s.clear();
    TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

// --- WiFi STA-drop recovery (#147) -------------------------------------------------
using tinkle::WifiLinkMonitor;
using tinkle::WifiAction;

// A momentary blip shorter than the grace window does NOT trigger a reconnect (no thrash).
void test_wifi_blip_under_grace_no_reconnect() {
    WifiLinkMonitor::Config cfg; cfg.graceMs = 8000; cfg.retryIntervalMs = 15000;
    WifiLinkMonitor m(cfg);
    TEST_ASSERT_EQUAL(WifiAction::None, m.update(true, 0));       // connected
    TEST_ASSERT_EQUAL(WifiAction::None, m.update(false, 1000));   // dropped
    TEST_ASSERT_EQUAL(WifiAction::None, m.update(false, 5000));   // still within grace
    TEST_ASSERT_EQUAL(WifiAction::None, m.update(true, 6000));    // recovered on its own
}

// A sustained drop reconnects after the grace, then again every retry interval.
void test_wifi_sustained_drop_reconnects_periodically() {
    WifiLinkMonitor::Config cfg; cfg.graceMs = 8000; cfg.retryIntervalMs = 15000;
    WifiLinkMonitor m(cfg);
    m.update(true, 0);
    m.update(false, 1000);                                        // drop at t=1s
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(false, 8000));   // grace not elapsed (7s down)
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, 9000));   // 8s down -> first reconnect
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(false, 20000));  // <15s since last try
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, 24000)); // 15s later -> retry again
}

// Re-association re-arms: a later drop starts its own fresh grace + retry cadence.
void test_wifi_reassociation_rearms() {
    WifiLinkMonitor::Config cfg; cfg.graceMs = 8000; cfg.retryIntervalMs = 15000;
    WifiLinkMonitor m(cfg);
    m.update(true, 0);
    m.update(false, 1000);
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, 9000));   // recovering
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(true, 12000));   // back up -> re-armed
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(false, 13000));  // new drop
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(false, 20000));  // within the fresh grace
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, 21500));  // fresh grace elapsed
}

// millis() wraparound (~49.7 days uptime): the (uint32_t)(now - x) idiom must still time
// the grace + retry correctly across the rollover.
void test_wifi_recovery_survives_millis_wrap() {
    WifiLinkMonitor::Config cfg; cfg.graceMs = 8000; cfg.retryIntervalMs = 15000;
    WifiLinkMonitor m(cfg);
    const uint32_t base = 0xFFFFFFF0u;                 // 16 ms before rollover
    m.update(true, base);
    m.update(false, base + 1000u);                     // drop just before wrap (wraps at +16)
    TEST_ASSERT_EQUAL(WifiAction::None,      m.update(false, base + 5000u));   // <8s down (wrapped)
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, base + 9000u));   // 8s down -> reconnect
    TEST_ASSERT_EQUAL(WifiAction::Reconnect, m.update(false, base + 24000u));  // +15s -> retry
}

// --- Distributed Watering (DEC-024) ------------------------------------------------
namespace {
DistributedConfig mkDist(uint16_t startMin, uint16_t endMin, uint16_t perZoneMin,
                         uint8_t fertCount, bool enabled = true) {
    DistributedConfig c;
    c.enabled = enabled; c.windowStartMin = startMin; c.windowEndMin = endMin;
    c.perZoneMin = perZoneMin; c.fertCount = fertCount;
    return c;
}
} // namespace

// Plan: 32 min/zone over 09:00–15:30 (540–930) with 3 zones -> 4 runs x 8 min, bookended.
void test_dist_plan_basic() {
    DistributedPlan p = computeDistributedPlan(mkDist(540, 930, 32, 1), 3);
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_EQUAL_UINT8(4, p.cycles);
    TEST_ASSERT_EQUAL_UINT16(480, p.runLenSec);          // 32*60/4
    TEST_ASSERT_EQUAL_UINT16(540, p.cycleStartMin[0]);   // first at window start
    TEST_ASSERT_EQUAL_UINT16(903, p.cycleStartMin[3]);   // last cycle ends by the window end
}

// Floor + cap: below the 7-min floor is invalid; a big budget caps at DIST_MAX_RUNS.
void test_dist_plan_floor_and_cap() {
    TEST_ASSERT_FALSE(computeDistributedPlan(mkDist(540, 930, 6, 0), 3).valid);   // < floor

    DistributedPlan one = computeDistributedPlan(mkDist(540, 930, 7, 0), 3);
    TEST_ASSERT_TRUE(one.valid);
    TEST_ASSERT_EQUAL_UINT8(1, one.cycles);
    TEST_ASSERT_EQUAL_UINT16(420, one.runLenSec);
    TEST_ASSERT_EQUAL_UINT16(540, one.cycleStartMin[0]);

    DistributedPlan cap = computeDistributedPlan(mkDist(540, 930, 60, 0), 3);   // 60/7=8 -> cap 6
    TEST_ASSERT_TRUE(cap.valid);
    TEST_ASSERT_EQUAL_UINT8(DIST_MAX_RUNS, cap.cycles);
    TEST_ASSERT_EQUAL_UINT16(600, cap.runLenSec);        // 60*60/6
}

// Over-subscription: 6 runs of 7 min can't fit a 30-min window -> invalid, emits nothing.
void test_dist_plan_oversubscribed_invalid() {
    TEST_ASSERT_FALSE(computeDistributedPlan(mkDist(540, 570, 42, 0), 3).valid);
}

// A cycle fires one run per live zone, back-to-back; fert rides the first fertCount cycles.
void test_dist_emits_cycle_all_zones_fert_first() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);   // Monday 09:00 = cycle 0
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c, 3);
    s.setDistributed(mkDist(540, 930, 32, 1));            // fert the first cycle only
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());
    TEST_ASSERT_EQUAL_UINT8(0, sink.reqs[0].zoneIndex);
    TEST_ASSERT_EQUAL_UINT8(1, sink.reqs[1].zoneIndex);
    TEST_ASSERT_EQUAL_UINT8(2, sink.reqs[2].zoneIndex);
    TEST_ASSERT_EQUAL_UINT32(480, sink.reqs[0].durationSec);
    TEST_ASSERT_TRUE(sink.reqs[0].fertigate);            // cycle 0 < fertCount 1
    TEST_ASSERT_TRUE(sink.reqs[2].fertigate);

    s.tick(121u * 60u * 1000u);                          // advance to cycle 1 (11:01)
    TEST_ASSERT_EQUAL_INT(6, (int)sink.reqs.size());
    TEST_ASSERT_FALSE(sink.reqs[3].fertigate);           // cycle 1 >= fertCount -> plain
}

// evalNow() re-entry within the same minute must NOT re-fire a cycle (cycle-index keyed).
void test_dist_idempotent_across_evalnow() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c, 3);
    s.setDistributed(mkDist(540, 930, 32, 0));
    s.tick(0);                                           // cycle 0 -> 3 runs
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());
    s.evalNow(0);                                        // same minute re-eval (config edit)
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());     // NOT 6
}

// Either/or: when Distributed is enabled, entry-schedule entries are dormant.
void test_dist_either_or_entry_dormant() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c, 3);
    s.add(mkEntry(2, 9, 0, DAILY, 999));                 // due at 09:00, distinct duration
    s.setDistributed(mkDist(540, 930, 32, 0));           // enabled -> replaces the schedule
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());     // only the distributed runs
    for (size_t i = 0; i < sink.reqs.size(); ++i)
        TEST_ASSERT_EQUAL_UINT32(480, sink.reqs[i].durationSec);   // none is the 999-s entry
}

// A refused cycle counts every dropped zone run (§13 drop + count).
void test_dist_dropped_counts() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink; sink.accept = false;
    Scheduler s(sink, c, 3);
    s.setDistributed(mkDist(540, 930, 32, 0));
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(0, (int)sink.reqs.size());
    TEST_ASSERT_EQUAL_UINT32(3, s.dropped());
}

// Enabled but INVALID (over-subscribed): distributed is not active, and the controller
// must NOT water nothing — the fixed schedule governs instead. (#142 review: no silent dry day.)
void test_dist_invalid_enabled_falls_back_to_schedule() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c, 3);
    s.add(mkEntry(1, 9, 0, DAILY, 300));                 // fixed entry due at 09:00
    s.setDistributed(mkDist(540, 570, 42, 0));           // enabled but over-subscribed -> invalid
    TEST_ASSERT_FALSE(s.distributedActive());
    s.tick(0);
    TEST_ASSERT_EQUAL_INT(1, (int)sink.reqs.size());     // the fixed entry ran, not distributed
    TEST_ASSERT_EQUAL_UINT8(1, sink.reqs[0].zoneIndex);
    TEST_ASSERT_EQUAL_UINT32(300, sink.reqs[0].durationSec);
}

// The fired-cycle bitmask resets at the day boundary: cycle 0 re-fires the next day.
void test_dist_day_boundary_resets_mask() {
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 9, 0, 0);   // Mon 09:00
    Clock c(src); c.begin(0);
    FakeRunSink sink;
    Scheduler s(sink, c, 3);
    s.setDistributed(mkDist(540, 930, 32, 0));
    s.tick(0);                                            // Mon cycle 0 -> 3 runs
    TEST_ASSERT_EQUAL_INT(3, (int)sink.reqs.size());
    s.tick(86400u * 1000u);                               // Tue 09:00 -> mask reset, cycle 0 again
    TEST_ASSERT_EQUAL_INT(6, (int)sink.reqs.size());
}

// Packed `dist` record survives a roundtrip.
void test_dist_config_pack_roundtrip() {
    DistributedConfig c = mkDist(540, 930, 32, 2);
    uint8_t blob[DIST_CONFIG_BYTES];
    packDistributedConfig(c, blob);
    DistributedConfig r = unpackDistributedConfig(blob);
    TEST_ASSERT_TRUE(r.enabled);
    TEST_ASSERT_EQUAL_UINT16(540, r.windowStartMin);
    TEST_ASSERT_EQUAL_UINT16(930, r.windowEndMin);
    TEST_ASSERT_EQUAL_UINT16(32, r.perZoneMin);
    TEST_ASSERT_EQUAL_UINT8(2, r.fertCount);
}

// --- Fertigation end-to-end (firmware spec §6 / #28) -----------------------------
// The §6 policy is decided in the Scheduler and actuated by RunController via the §4
// diverter step. These tests exercise the REAL stack (Scheduler -> RunController ->
// ValveDriver over FakeGpio) rather than a fake sink, locking the criterion that the
// first auto run of the day routes THROUGH the Dosatron and the rest route AROUND.

void test_sched_rc_fert_routing() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FakeWallClock src; src.available = true;
    src.epochVal = epochFromCivil(2026, 6, 8, 6, 0, 0);   // Monday 06:00
    Clock clock(src); clock.begin(0);
    Scheduler s(rc, clock);
    s.add(mkEntry(0, 6, 0, DAILY, 60));                   // first auto run of the day -> fert
    s.add(mkEntry(1, 6, 5, DAILY, 60));                   // later auto run -> no fert

    // 06:00 — scheduler enqueues the fert run; RunController commands the legs to fert.
    s.tick(0);
    TEST_ASSERT_FALSE(rc.isIdle());
    TEST_ASSERT_TRUE(vd.diverterFert());                  // routed through the Dosatron
    uint32_t now = pump(rc, 0, 5, 80000, [&]{ return rc.isIdle(); });
    TEST_ASSERT_TRUE(rc.isIdle());

    // 06:05 — second auto run of the same day: no fert, diverter travels back to plain.
    now = 300000;                                         // 06:05 in free-run ms
    s.tick(now);
    TEST_ASSERT_FALSE(rc.isIdle());
    TEST_ASSERT_FALSE(vd.diverterFert());                 // bypassed (plain)
}

// --- Persistence: flow K (§7 / #34) ----------------------------------------------
void test_persist_pulses_per_gallon() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    TEST_ASSERT_EQUAL_FLOAT(Persistence::DEFAULT_PULSES_PER_GALLON, p.pulsesPerGallon());  // seed
    p.setPulsesPerGallon(382.5f);
    TEST_ASSERT_EQUAL_FLOAT(382.5f, p.pulsesPerGallon());
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("k_ppg"));
    p.setPulsesPerGallon(382.5f);                       // unchanged -> no write
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("k_ppg"));
    p.setPulsesPerGallon(0.0f);                         // invalid -> ignored, no write
    TEST_ASSERT_EQUAL_FLOAT(382.5f, p.pulsesPerGallon());
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("k_ppg"));
    // Survives reboot.
    Persistence p2(s, 3);
    p2.begin();
    TEST_ASSERT_EQUAL_FLOAT(382.5f, p2.pulsesPerGallon());
}

// --- FlowMonitor (firmware spec §7 / #34) ----------------------------------------
using tinkle::FlowMonitor;

void test_flow_gallons_from_k() {
    FlowMonitor fm(450.0f);                 // K = 450 pulses/gal
    fm.begin(0, 0);
    fm.tick(450, 1000);                     // one gallon's worth
    TEST_ASSERT_EQUAL_FLOAT(1.0f, fm.gallons());
    fm.tick(1125, 2000);                    // 2.5 gal total
    TEST_ASSERT_EQUAL_FLOAT(2.5f, fm.gallons());
    TEST_ASSERT_EQUAL_UINT32(1125, fm.pulsesSinceReset());
}

void test_flow_k_guard() {
    FlowMonitor fm(450.0f);
    fm.setK(0.0f);                          // rejected
    TEST_ASSERT_EQUAL_FLOAT(450.0f, fm.k());
    fm.setK(-5.0f);                         // rejected
    TEST_ASSERT_EQUAL_FLOAT(450.0f, fm.k());
    fm.setK(300.0f);                        // accepted
    TEST_ASSERT_EQUAL_FLOAT(300.0f, fm.k());
}

void test_flow_rate_gpm() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);                         // sample @0: 0 pulses
    fm.tick(450, 1000);                     // +450 pulses in 1 s = 1 gal/s = 60 GPM
    fm.tick(900, 2000);
    fm.tick(1350, 3000);                    // window 0..3000: 1350 pulses = 3 gal over 0.05 min
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, fm.rateGPM());
}

void test_flow_rate_decays_when_flow_stops() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);
    for (uint32_t t = 1000; t <= 3000; t += 1000) fm.tick(t * 0.45f, t);  // flowing
    TEST_ASSERT_TRUE(fm.rateGPM() > 0.0f);
    // Flow stops: pulse count frozen, but ticks continue. After the window fills with the
    // frozen count, the rolling rate reads ~0 — this is what the §7 no-flow check (#35) keys on.
    uint32_t frozen = 1350;
    for (uint32_t t = 4000; t <= 12000; t += 1000) fm.tick(frozen, t);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fm.rateGPM());
    TEST_ASSERT_EQUAL_FLOAT(3.0f, fm.gallons());        // 1350/450, still the run total
}

void test_flow_reset_accumulation() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);
    fm.tick(900, 1000);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, fm.gallons());
    fm.resetAccumulation(900, 2000);        // new run starts at the current count
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fm.gallons());
    fm.tick(1350, 3000);                     // +450 since reset
    TEST_ASSERT_EQUAL_FLOAT(1.0f, fm.gallons());
}

void test_flow_single_sample_zero_rate() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);                         // exactly one sample in the window
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fm.rateGPM());
}

void test_flow_subhz_ticks_update_gallons() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);
    // Production cadence: tick every ~10 ms. gallons() tracks every call, but no second ring
    // sample is taken before the 1 Hz boundary, so rateGPM stays 0 until ~1 s elapses.
    for (uint32_t t = 10; t <= 100; t += 10) fm.tick((t / 10) * 90, t);   // 900 pulses by t=100
    TEST_ASSERT_EQUAL_FLOAT(2.0f, fm.gallons());     // 900/450, updated mid-second
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fm.rateGPM());     // still one sample
}

void test_flow_full_ring_steady_rate() {
    FlowMonitor fm(450.0f);
    fm.begin(0, 0);
    // Steady 60 GPM (450 pulses/s) sampled at 1 Hz for 12 s — past the 8-slot ring, so the
    // head wraps and oldest = sampleHead_. Rate must stay correct after the wrap.
    for (uint32_t t = 1; t <= 12; ++t) fm.tick(t * 450, t * 1000);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 60.0f, fm.rateGPM());
}

// --- FlowFaultDetector (firmware spec §7/§14 / #35) ------------------------------
using tinkle::FlowFaultDetector;

namespace {
FlowFaultDetector::Config ffCfg() {
    FlowFaultDetector::Config c;
    c.graceMs        = 2000;     // short windows so the tests run fast
    c.minRunningGPM  = 0.1f;
    c.idleFaultPulses = 50;
    c.idleWindowMs   = 1000;
    c.drainQuietMs     = 500;    // #124 drain gate, shortened to match
    c.drainQuietPulses = 2;
    c.drainCapMs       = 3000;
    return c;
}
} // namespace

// No-flow arms only AFTER the grace window, and only while RUNNING at ~0 GPM.
void test_ff_no_flow_after_grace() {
    FlowFaultDetector d(ffCfg());
    TEST_ASSERT_EQUAL(Fault::None,   d.update(RunState::Running, 0.0f, 0, 0));      // run start
    TEST_ASSERT_EQUAL(Fault::None,   d.update(RunState::Running, 0.0f, 0, 1999));   // pre-grace
    TEST_ASSERT_EQUAL(Fault::NoFlow, d.update(RunState::Running, 0.0f, 0, 2000));   // grace elapsed
}

// Healthy flow during RUNNING never trips, even well past the grace window.
void test_ff_no_flow_healthy_rate() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Running, 5.0f, 0, 0);
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Running, 5.0f, 1000, 5000));
}

// Unexpected flow: pulses climbing past the threshold over an idle window -> fault.
void test_ff_unexpected_idle_flow() {
    FlowFaultDetector d(ffCfg());
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 100, 0));         // baseline 100
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 200, 500));       // window not up
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow,
                      d.update(RunState::Idle, 0.0f, 200, 1000));                   // delta 100 > 50
}

// Sub-threshold idle drift never faults; the window slides so it can't accumulate across windows.
void test_ff_idle_subthreshold_no_fault() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Idle, 0.0f, 0, 0);                                           // baseline 0
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 10, 1000));       // +10 < 50, slide
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 20, 2000));       // +10 from slid base
}

// Transition states arm NEITHER check — flow legitimately ramps/trails there (S6 carry-forward).
void test_ff_transition_states_never_fault() {
    FlowFaultDetector d(ffCfg());
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::StartPump, 0.0f, 0, 99999));   // zero flow, late
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::StopPump,  0.0f, 0, 99999));
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::CloseZone, 0.0f, 9000, 99999));// high flow, fine
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Settle,    0.0f, 9000, 99999));
}

// The grace clock restarts on each RUNNING entry — a second run gets its own grace window.
void test_ff_grace_resets_per_run() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Running, 5.0f, 0, 0);
    d.update(RunState::Running, 5.0f, 100, 3000);          // run 1, flowing
    d.update(RunState::Idle,    0.0f, 100, 5000);          // back to idle
    d.update(RunState::Running, 0.0f, 100, 10000);         // run 2 starts -> grace clock = 10000
    TEST_ASSERT_EQUAL(Fault::None,   d.update(RunState::Running, 0.0f, 100, 11999)); // <2 s into run 2
    TEST_ASSERT_EQUAL(Fault::NoFlow, d.update(RunState::Running, 0.0f, 100, 12000)); // grace elapsed
}

// begin() seeds the idle window past boot: a slow setup() (> idleWindowMs) with pulses
// counted during it must NOT read as unexpected flow on the first tick.
void test_ff_begin_seeds_boot_window() {
    FlowFaultDetector d(ffCfg());
    d.begin(500, 3000);                                                             // late boot, 500 pulses
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 500, 3000));      // first tick: no boot pseudo-window
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 500, 4000));      // quiet window from the seed
}

// Pulses accrued through the close sequence (trailing flow in StopPump/Settle) are excluded
// from the first idle window: the Idle edge opens the #124 drain gate, and the window only
// baselines once the meter has quiesced — no false UnexpectedFlow after a run.
void test_ff_post_run_idle_rebaseline() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Running,  5.0f, 1000, 0);
    d.update(RunState::StopPump, 1.0f, 1200, 5000);                                 // flow trailing off
    d.update(RunState::Settle,   0.2f, 1300, 5500);                                 // +300 since run > threshold
    d.update(RunState::Idle,     0.0f, 1300, 6000);                                 // edge: drain gate opens
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 1300, 6500));     // quiet 500 ms -> armed
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 1300, 7500));     // full window, flat -> clean
}

// The #124 repro: a wet run's draindown keeps the meter ticking well into IDLE —
// far past the idle threshold — then decays. The drain gate must absorb the tail,
// and the idle check must still be LIVE afterward (a later real flow trips it).
void test_ff_trailing_draindown_no_fault_then_still_armed() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Running,  5.0f, 1000, 0);
    d.update(RunState::StopPump, 1.0f, 1500, 3000);
    d.update(RunState::Settle,   0.5f, 1600, 3500);
    // IDLE entry with the draindown still running: +150 pulses over the first
    // second — triple the 50-pulse idle threshold; the old code latched here.
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.3f, 1700, 4000));   // edge
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.3f, 1760, 4250));
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.2f, 1810, 4500));
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.1f, 1840, 4750));   // decaying
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 1850, 5000));
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 1851, 5500));   // quiet 500 ms -> armed
    TEST_ASSERT_EQUAL(Fault::None, d.update(RunState::Idle, 0.0f, 1851, 6500));   // clean window slides
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow,
                      d.update(RunState::Idle, 1.0f, 1960, 7500));                // real flow still trips
}

// The other half of #124's acceptance: flow that NEVER decays (stuck-open valve,
// burst) must still latch. The gate caps at drainCapMs, arms from a fresh baseline,
// and the sustained flow trips the check within one idle window.
void test_ff_never_decaying_flow_still_faults() {
    FlowFaultDetector d(ffCfg());
    d.update(RunState::Running, 5.0f, 1000, 0);
    d.update(RunState::Settle,  2.0f, 1500, 3500);
    Fault f = Fault::None;
    uint32_t pulses = 1600;
    uint32_t trippedAt = 0;
    for (uint32_t now = 4000; now <= 9000 && f == Fault::None; now += 250) {
        pulses += 20;                                             // 80 pulses/s, forever
        f = d.update(RunState::Idle, 2.0f, pulses, now);
        trippedAt = now;
    }
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow, f);
    // Latency is bounded: cap (3000) + one idle window (1000) past the IDLE edge.
    TEST_ASSERT_TRUE(trippedAt <= 4000 + 3000 + 1000);
}

// #141: at the PRODUCTION default config (K≈1670), the idle-flow threshold is ~1 GPM, not
// ~0.36 GPM. A decayed draindown tail (~0.36 GPM ≈ 50 pulses / 5 s) no longer latches E2;
// a welded pump relay (~1.45 GPM ≈ 202 pulses / 5 s) still does. begin() arms the boot-idle
// window directly, so this exercises the threshold itself, not the #124 drain gate.
void test_ff_default_threshold_ignores_draindown_traps_welded_relay() {
    FlowFaultDetector::Config prod;                    // production seeds

    FlowFaultDetector tail(prod);                      // decayed draindown tail
    tail.begin(0, 0);
    TEST_ASSERT_EQUAL(Fault::None, tail.update(RunState::Idle, 0.36f, 0, 0));
    TEST_ASSERT_EQUAL(Fault::None, tail.update(RunState::Idle, 0.36f, 50, 5000));      // +50 < 139

    FlowFaultDetector welded(prod);                    // welded/stuck pump relay
    welded.begin(0, 0);
    TEST_ASSERT_EQUAL(Fault::None, welded.update(RunState::Idle, 1.45f, 0, 0));
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow,
                      welded.update(RunState::Idle, 1.45f, 202, 5000));                // +202 > 139
}

// End-to-end: a no-flow verdict routed to RunController slams the safe state and latches (§14).
void test_ff_integration_no_flow_faults_rc() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FlowFaultDetector d(ffCfg());
    RunRequest req; req.zoneIndex = 0; req.durationSec = 60; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });

    Fault f = Fault::None;
    for (uint32_t t = now; t <= now + 3000 && f == Fault::None; t += 100) {
        f = d.update(rc.state(), 0.0f, 0, t);            // zero flow throughout
        if (f != Fault::None) rc.raiseFault(f, t);
    }
    TEST_ASSERT_EQUAL(Fault::NoFlow, f);
    TEST_ASSERT_EQUAL(RunState::Fault, rc.state());
    TEST_ASSERT_EQUAL(Fault::NoFlow, rc.activeFault());
    TEST_ASSERT_FALSE(vd.pumpIsOn());
    TEST_ASSERT_FALSE(g.level[ZF0]);                     // safe state: zone de-energized
}

// --- CalibrationController (firmware spec §7 / #36) -------------------------------
// The calibration state machine + K math, host-driven end to end: real RunController
// + ValveDriver on the fake GPIO, real Persistence on the fake store, injected pulse
// counts and an explicit clock. Actuation stays behind RunController throughout.

using tinkle::CalibrationController;
using CalPhase  = CalibrationController::Phase;
using CalResult = CalibrationController::FinishResult;

namespace {

CalibrationController::Config calCfg() {
    CalibrationController::Config c;
    c.runSec     = 60;       // short bounded run for tests
    c.minGallons = 0.25f;
    c.minK       = 50.0f;
    c.maxK       = 5000.0f;
    return c;
}

// One harness per test: the full chain a calibration touches.
struct CalRig {
    ValveDriver   vd;
    RunController rc;
    FakeKvStore   kv;
    Persistence   store;
    FlowMonitor   flow;
    CalibrationController cal;

    CalRig()
        : vd(g, cfg), rc(vd, makeRunCfg()), store(kv, 2),
          flow(Persistence::DEFAULT_PULSES_PER_GALLON),
          cal(rc, store, flow, calCfg()) {
        rc.begin(0);
        store.begin();
    }

    // Mirror the main loop: tick the controller then the calibration, stepping the
    // clock, until pred() holds or the budget runs out. Pulses are injected by the
    // caller via the pulse reference (a "live counter" the test mutates).
    template <class Pred>
    uint32_t pump(uint32_t now, uint32_t stepMs, uint32_t budgetMs,
                  const uint32_t& pulses, Pred pred) {
        uint32_t spent = 0;
        while (!pred() && spent <= budgetMs) {
            rc.tick(now);
            cal.tick(rc.state(), pulses, now);
            now += stepMs;
            spent += stepMs;
        }
        return now;
    }
};

} // namespace

// start() requests a bounded run with the diverter plain and baselines the tally.
static void test_cal_start_bounded_plain_run() {
    CalRig r;
    uint32_t pulses = 1000;                       // counter needn't start at zero
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    TEST_ASSERT_EQUAL(CalPhase::Running, r.cal.phase());

    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(r.vd.pumpIsOn());
    TEST_ASSERT_TRUE(g.level[ZF0]);               // zone open
    TEST_ASSERT_FALSE(r.vd.diverterFert());       // diverter plain (§7)
    // Bounded by its own ceiling, not swMaxRuntimeSec.
    TEST_ASSERT_TRUE(r.rc.remainingSec(now) <= 60);
    TEST_ASSERT_EQUAL_UINT32(0, r.cal.pulsesCounted());
}

// No queueing: refuse when another run is active, and one calibration at a time.
static void test_cal_start_refused_when_busy() {
    CalRig r;
    RunRequest req; req.zoneIndex = 1; req.durationSec = 30;
    TEST_ASSERT_TRUE(r.rc.requestRun(req, 0));
    TEST_ASSERT_FALSE(r.cal.start(0, 0, 0));      // RunController busy
    TEST_ASSERT_EQUAL(CalPhase::Idle, r.cal.phase());

    CalRig r2;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r2.cal.start(0, pulses, 0));
    TEST_ASSERT_FALSE(r2.cal.start(1, pulses, 0)); // already calibrating
}

// Happy path: run completes, tally freezes at SETTLE, finish computes + persists K.
static void test_cal_happy_path_k_to_nvs() {
    CalRig r;
    uint32_t pulses = 500;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });

    pulses += 900;                                 // water flows during the run
    now = r.pump(now, 1000, 70000, pulses,
                 [&]{ return r.cal.phase() == CalPhase::Awaiting; });
    TEST_ASSERT_EQUAL_UINT32(900, r.cal.pulsesCounted());

    pulses += 300;                                 // post-settle pulses must NOT count
    now = r.pump(now, 100, 1000, pulses, [&]{ return r.rc.isIdle(); });
    r.cal.tick(r.rc.state(), pulses, now);
    TEST_ASSERT_EQUAL_UINT32(900, r.cal.pulsesCounted());

    // 900 / 1.5 = 600 — deliberately NOT the default K, or Persistence's
    // write-on-change would (correctly) skip the flash write and hide the path.
    TEST_ASSERT_EQUAL(CalResult::Ok, r.cal.finish(1.5f, pulses, now));
    TEST_ASSERT_EQUAL_FLOAT(600.0f, r.cal.lastK());
    TEST_ASSERT_EQUAL_FLOAT(600.0f, r.flow.k());               // live monitor updated
    TEST_ASSERT_EQUAL_FLOAT(600.0f, r.kv.getFloat("k_ppg", 0)); // NVS (§8)
    TEST_ASSERT_EQUAL(CalPhase::Idle, r.cal.phase());
    TEST_ASSERT_FALSE(r.rc.isFaulted());
}

// finish() mid-run stops the run (graceful unwind) and still judges the tally.
static void test_cal_finish_mid_run_stops() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    pulses += 600;
    r.cal.tick(r.rc.state(), pulses, now);

    TEST_ASSERT_EQUAL(CalResult::Ok, r.cal.finish(1.0f, pulses, now));
    TEST_ASSERT_EQUAL_FLOAT(600.0f, r.flow.k());
    now = r.pump(now, 100, 70000, pulses, [&]{ return r.rc.isIdle(); });
    TEST_ASSERT_TRUE(r.rc.isIdle());               // closed everything, no fault
    TEST_ASSERT_FALSE(r.vd.pumpIsOn());
}

// Absurd measured volume -> FAULT_CAL_RANGE, K untouched (NVS and live).
static void test_cal_reject_bad_volume() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    pulses += 900;
    r.cal.tick(r.rc.state(), pulses, now);

    TEST_ASSERT_EQUAL(CalResult::Rejected, r.cal.finish(0.1f, pulses, now));
    TEST_ASSERT_EQUAL(Fault::CalRange, r.rc.activeFault());   // §14 latch
    TEST_ASSERT_FALSE(r.vd.pumpIsOn());                       // safe state slammed
    TEST_ASSERT_EQUAL_FLOAT(Persistence::DEFAULT_PULSES_PER_GALLON, r.flow.k());
    TEST_ASSERT_EQUAL_INT(0, r.kv.writesTo("k_ppg"));         // nothing hit flash
    TEST_ASSERT_EQUAL(CalPhase::Idle, r.cal.phase());
}

// K outside sanity bounds (both sides) -> FAULT_CAL_RANGE, K untouched. A zero tally
// lands here too (K = 0 < minK) — no divide-by-zero path to a write.
static void test_cal_reject_k_out_of_range() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    pulses += 20;                                  // 20 pulses / 1 gal -> K = 20 < 50
    r.cal.tick(r.rc.state(), pulses, now);
    TEST_ASSERT_EQUAL(CalResult::Rejected, r.cal.finish(1.0f, pulses, now));
    TEST_ASSERT_EQUAL(Fault::CalRange, r.rc.activeFault());
    TEST_ASSERT_EQUAL_INT(0, r.kv.writesTo("k_ppg"));

    CalRig r2;
    pulses = 0;
    TEST_ASSERT_TRUE(r2.cal.start(0, pulses, 0));
    now = r2.pump(0, 5, 8000, pulses,
                  [&]{ return r2.rc.state() == RunState::Running; });
    pulses += 10000;                               // 10000 / 0.5 -> K = 20000 > 5000
    r2.cal.tick(r2.rc.state(), pulses, now);
    TEST_ASSERT_EQUAL(CalResult::Rejected, r2.cal.finish(0.5f, pulses, now));
    TEST_ASSERT_EQUAL(Fault::CalRange, r2.rc.activeFault());
}

// finish() with no calibration in progress: refused, but no fault raised.
static void test_cal_finish_not_calibrating() {
    CalRig r;
    TEST_ASSERT_EQUAL(CalResult::NotCalibrating, r.cal.finish(2.0f, 0, 0));
    TEST_ASSERT_FALSE(r.rc.isFaulted());
}

// A fault during the cal run voids the calibration; the original fault owns the story.
static void test_cal_run_fault_aborts() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    r.rc.raiseFault(Fault::NoFlow, now);
    r.cal.tick(r.rc.state(), pulses, now);
    TEST_ASSERT_EQUAL(CalPhase::Idle, r.cal.phase());
    TEST_ASSERT_EQUAL(CalResult::NotCalibrating, r.cal.finish(2.0f, pulses, now));
    TEST_ASSERT_EQUAL(Fault::NoFlow, r.rc.activeFault());     // not overwritten
}

// cancel(): graceful abort — run unwinds, no fault, no K write.
static void test_cal_cancel() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    r.cal.cancel(now);
    TEST_ASSERT_EQUAL(CalPhase::Idle, r.cal.phase());
    now = r.pump(now, 100, 70000, pulses, [&]{ return r.rc.isIdle(); });
    TEST_ASSERT_TRUE(r.rc.isIdle());
    TEST_ASSERT_FALSE(r.rc.isFaulted());
    TEST_ASSERT_EQUAL_INT(0, r.kv.writesTo("k_ppg"));
}

// The frozen tally survives a queued run chaining SETTLE -> next run without IDLE:
// the second run's water must not inflate the calibration.
static void test_cal_tally_immune_to_chained_run() {
    CalRig r;
    uint32_t pulses = 0;
    TEST_ASSERT_TRUE(r.cal.start(0, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses,
                          [&]{ return r.rc.state() == RunState::Running; });
    pulses += 900;

    RunRequest next; next.zoneIndex = 1; next.durationSec = 30;
    TEST_ASSERT_TRUE(r.rc.requestRun(next, now));   // queues behind the cal run

    now = r.pump(now, 1000, 70000, pulses,
                 [&]{ return r.cal.phase() == CalPhase::Awaiting; });
    now = r.pump(now, 1000, 70000, pulses,
                 [&]{ return r.rc.state() == RunState::Running; });  // queued run live
    pulses += 5000;                                 // the OTHER run's water
    r.cal.tick(r.rc.state(), pulses, now);
    TEST_ASSERT_EQUAL_UINT32(900, r.cal.pulsesCounted());
    TEST_ASSERT_EQUAL(CalResult::Ok, r.cal.finish(2.0f, pulses, now));
    TEST_ASSERT_EQUAL_FLOAT(450.0f, r.flow.k());
}

// ----------------------------------------------------------------------------
// Watchdog handshake (firmware spec §9, Unit B / DEC-016). Both halves of the one
// protocol, tested against the same constants: WatchdogTrip is the EXACT unit the
// attiny85 binary compiles (src/core/watchdog_trip.h), Watchdog is the ESP32
// emitter/verdict side, and the integration test locks the DEC-003 criterion —
// the heartbeat exists ONLY while the pump is commanded.
// ----------------------------------------------------------------------------

using tinkle::FaultManager;
using tinkle::Watchdog;
using tinkle::WatchdogTrip;

namespace {

WatchdogTrip::Config makeTripCfg() {
    WatchdogTrip::Config c;
    c.hbTimeoutMs = 2000;       // §15 HB_TIMEOUT_MS
    c.hardMaxMs   = 60000;      // 1 min ceiling keeps tests fast (real: 30 min, §15)
    return c;
}

// Feed a square wave: toggle `level` every halfMs, polling the trip logic at a
// 50 ms tick (the ATtiny loop is far faster; coarse polling is the harder case).
uint32_t hbBeat(WatchdogTrip& wt, bool& level, uint32_t now, uint32_t halfMs, uint32_t spanMs) {
    const uint32_t end = now + spanMs;
    uint32_t nextToggle = now;
    while ((int32_t)(end - now) > 0) {
        if ((int32_t)(now - nextToggle) >= 0) { level = !level; nextToggle += halfMs; }
        wt.tick(level, now);
        now += 50;
    }
    return now;
}

// Hold the line still for spanMs.
uint32_t hbQuiet(WatchdogTrip& wt, bool level, uint32_t now, uint32_t spanMs) {
    const uint32_t end = now + spanMs;
    while ((int32_t)(end - now) > 0) { wt.tick(level, now); now += 50; }
    return now;
}

constexpr uint8_t HB_PIN = 21;   // synthetic heartbeat pin, distinct from valve pins

// The two halves of the protocol live in separate headers; tie their §15 defaults
// here, where both are visible. The toggle period must sit well under the quiet
// timeout (4x margin) or the relay chatters mid-run on ordinary loop jitter.
static_assert(Watchdog::Config{}.heartbeatMs * 4 <= WatchdogTrip::Config{}.hbTimeoutMs,
              "HEARTBEAT_MS must stay well under HB_TIMEOUT_MS (4x margin)");

} // namespace

// Power-up default is DISARMED, and a line frozen at any level never arms — only
// an edge does (§9: fail dry on watchdog reboot).
static void test_wdt_powerup_disarmed_frozen_line_never_arms() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(true, 0);                       // captured mid-stream HIGH: not an edge
    TEST_ASSERT_FALSE(wt.armed());
    TEST_ASSERT_FALSE(wt.tripped());
    uint32_t now = hbQuiet(wt, true, 0, 10000);
    TEST_ASSERT_FALSE(wt.armed());
    TEST_ASSERT_FALSE(wt.tripped());
    (void)now;
}

// The first heartbeat edge arms; edges within the timeout hold it armed.
static void test_wdt_arms_on_edge_and_holds_with_beat() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(false, 0);
    bool level = false;
    uint32_t now = hbQuiet(wt, level, 0, 500);
    TEST_ASSERT_FALSE(wt.armed());
    now = hbBeat(wt, level, now, 250, 30000);   // §15 cadence, half the test ceiling
    TEST_ASSERT_TRUE(wt.armed());
    TEST_ASSERT_FALSE(wt.tripped());
}

// Heartbeat quiet while armed disarms SILENTLY within the timeout — a clean run
// end is not a trip (§17 item 2 is about the relay opening, which disarm does).
static void test_wdt_clean_disarm_no_trip() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(false, 0);
    bool level = false;
    uint32_t now = hbBeat(wt, level, 0, 250, 5000);
    TEST_ASSERT_TRUE(wt.armed());
    now = hbQuiet(wt, level, now, 2100);        // just past HB_TIMEOUT_MS
    TEST_ASSERT_FALSE(wt.armed());
    TEST_ASSERT_FALSE(wt.tripped());
}

// §17 item 3: a beating-but-overrun ESP32 is cut at HARD_MAX_RUNTIME on the
// ATtiny's own clock, TRIPPED asserts, and the lockout HOLDS while the ESP32
// keeps claiming run-active.
static void test_wdt_hard_max_lockout_holds_under_beat() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(false, 0);
    bool level = false;
    uint32_t now = hbBeat(wt, level, 0, 250, 61000);    // past the 60 s test ceiling
    TEST_ASSERT_FALSE(wt.armed());
    TEST_ASSERT_TRUE(wt.tripped());
    now = hbBeat(wt, level, now, 250, 10000);           // still beating — still locked
    TEST_ASSERT_FALSE(wt.armed());
    TEST_ASSERT_TRUE(wt.tripped());
}

// Lockout releases only after the heartbeat goes quiet for the timeout (the ESP32
// latched FAULT_WATCHDOG, safed, stopped beating) — the §14 resolved signal.
static void test_wdt_lockout_releases_after_quiet() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(false, 0);
    bool level = false;
    uint32_t now = hbBeat(wt, level, 0, 250, 61000);
    TEST_ASSERT_TRUE(wt.tripped());
    now = hbQuiet(wt, level, now, 2100);
    TEST_ASSERT_FALSE(wt.tripped());
    TEST_ASSERT_FALSE(wt.armed());
}

// A clean disarm resets the runtime ceiling: two back-to-back runs of 40 s each
// (total 80 s > the 60 s ceiling) must both survive — HARD_MAX is per armed
// period, which the per-run heartbeat window makes per run.
static void test_wdt_ceiling_resets_per_arm() {
    WatchdogTrip wt(makeTripCfg());
    wt.begin(false, 0);
    bool level = false;
    uint32_t now = hbBeat(wt, level, 0, 250, 40000);
    TEST_ASSERT_TRUE(wt.armed());
    now = hbQuiet(wt, level, now, 2500);                // run gap >> HB_TIMEOUT
    TEST_ASSERT_FALSE(wt.armed());
    now = hbBeat(wt, level, now, 250, 40000);
    TEST_ASSERT_TRUE(wt.armed());                       // would be Lockout if cumulative
    TEST_ASSERT_FALSE(wt.tripped());
}

// All interval math is uint32_t subtraction: arm, beat, and disarm across the
// ~49.7-day millis() wrap.
static void test_wdt_millis_rollover() {
    WatchdogTrip wt(makeTripCfg());
    const uint32_t start = 0xFFFFFFFFu - 5000u;
    wt.begin(false, start);
    bool level = false;
    uint32_t now = hbBeat(wt, level, start, 250, 20000);   // crosses the wrap
    TEST_ASSERT_TRUE(wt.armed());
    TEST_ASSERT_FALSE(wt.tripped());
    now = hbQuiet(wt, level, now, 2100);
    TEST_ASSERT_FALSE(wt.armed());
}

// ESP32 side: the heartbeat is emitted ONLY while the pump is commanded
// (START_PUMP/RUNNING) — every other state is silent and parks the line LOW.
// This is the DEC-003 criterion at module level.
static void test_watchdog_emits_only_in_pump_window() {
    Watchdog wd(g, HB_PIN, Watchdog::Config{});
    wd.begin(0);
    TEST_ASSERT_FALSE(g.level[HB_PIN]);

    const RunState silent[] = { RunState::Idle, RunState::PrepDiverter,
                                RunState::OpenZone, RunState::StopPump,
                                RunState::CloseZone, RunState::Settle,
                                RunState::Fault };
    uint32_t now = 0;
    for (RunState s : silent) {
        const int before = g.writesTo(HB_PIN);
        for (uint32_t t = 0; t < 1000; t += 50) { wd.tick(s, false, now); now += 50; }
        TEST_ASSERT_EQUAL_INT(before, g.writesTo(HB_PIN));   // not one write
        TEST_ASSERT_FALSE(wd.emitting());
    }

    wd.tick(RunState::StartPump, false, now);
    TEST_ASSERT_TRUE(wd.emitting());
    TEST_ASSERT_TRUE(g.level[HB_PIN]);          // first edge fires immediately
}

// Toggle cadence: an edge on window entry, then one every HEARTBEAT_MS; leaving
// the window parks the line LOW.
static void test_watchdog_toggle_cadence_and_park() {
    Watchdog wd(g, HB_PIN, Watchdog::Config{});   // 250 ms
    wd.begin(0);
    const int baseline = g.writesTo(HB_PIN);      // begin()'s park write

    uint32_t now = 1000;
    for (uint32_t t = 0; t <= 1000; t += 50) { wd.tick(RunState::Running, false, now); now += 50; }
    // Edges at t=0 (entry), 250, 500, 750, 1000 — five toggles, alternating levels.
    TEST_ASSERT_EQUAL_INT(baseline + 5, g.writesTo(HB_PIN));

    wd.tick(RunState::Settle, false, now);        // window exit
    TEST_ASSERT_FALSE(wd.emitting());
    TEST_ASSERT_FALSE(g.level[HB_PIN]);
    TEST_ASSERT_EQUAL_INT(baseline + 6, g.writesTo(HB_PIN));   // the park write

    now += 1000;                                  // silence stays silent
    wd.tick(RunState::Idle, false, now);
    TEST_ASSERT_EQUAL_INT(baseline + 6, g.writesTo(HB_PIN));
}

// Trip verdicts: a CONFIRMED assertion during ANY active state is FAULT_WATCHDOG
// (water staged or moving); asserted while IDLE or already FAULTed is not a fault —
// the §4 pre-open gate covers new runs and the line self-releases. tripConfirmMs=0
// here isolates the state matrix; qualification has its own test below.
static void test_watchdog_trip_verdicts() {
    Watchdog::Config c; c.tripConfirmMs = 0;
    Watchdog wd(g, HB_PIN, c);
    wd.begin(0);
    TEST_ASSERT_EQUAL(Fault::Watchdog, wd.tick(RunState::Running,      true, 100));
    TEST_ASSERT_EQUAL(Fault::Watchdog, wd.tick(RunState::OpenZone,     true, 200));
    TEST_ASSERT_EQUAL(Fault::Watchdog, wd.tick(RunState::PrepDiverter, true, 300));
    TEST_ASSERT_EQUAL(Fault::None,     wd.tick(RunState::Idle,         true, 400));
    TEST_ASSERT_EQUAL(Fault::None,     wd.tick(RunState::Fault,        true, 500));
    TEST_ASSERT_EQUAL(Fault::None,     wd.tick(RunState::Running,      false, 600));
}

// Trip qualification (DEC-023, the 2026-07-09 field false positive): a single
// glitched sample — or any assertion shorter than tripConfirmMs — produces no
// verdict and never reaches tripConfirmed(); a held assertion confirms and stays
// confirmed; a released read resets the clock so bursts can't accumulate.
static void test_watchdog_trip_qualification_filters_glitches() {
    Watchdog wd(g, HB_PIN, Watchdog::Config{});   // tripConfirmMs = 100
    wd.begin(0);
    // One glitched sample mid-run (the field event): no verdict, no confirm.
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, true,  1000));
    TEST_ASSERT_FALSE(wd.tripConfirmed());
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, false, 1010));
    // A 90 ms burst: still under the bar.
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, true,  2000));
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, true,  2090));
    TEST_ASSERT_FALSE(wd.tripConfirmed());
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, false, 2100));   // resets
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, true,  2110));   // fresh clock
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Running, true,  2200));   // 90 ms again
    // A genuine lockout holds the line: confirms at the 100 ms bar and stays.
    TEST_ASSERT_EQUAL(Fault::Watchdog, wd.tick(RunState::Running, true, 2210));
    TEST_ASSERT_TRUE(wd.tripConfirmed());
    TEST_ASSERT_EQUAL(Fault::Watchdog, wd.tick(RunState::Running, true, 3000));
    // Idle: confirmed but not a verdict — the pre-open gate consumes tripConfirmed().
    TEST_ASSERT_EQUAL(Fault::None, wd.tick(RunState::Idle, true, 3100));
    TEST_ASSERT_TRUE(wd.tripConfirmed());
}

// End-to-end DEC-003 lock: drive a real run through RunController with the
// Watchdog on the same FakeGpio and assert every heartbeat write happens while
// the pump FET is commanded on — and none after it drops.
static void test_watchdog_full_run_heartbeat_within_pump() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    Watchdog wd(g, HB_PIN, Watchdog::Config{});
    wd.begin(0);
    const int baseline = g.writesTo(HB_PIN);

    RunRequest req; req.zoneIndex = 0; req.durationSec = 2;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));

    uint32_t now = 0;
    bool sawBeat = false;
    while (!(rc.isIdle() && now > 0) && now < 20000) {
        rc.tick(now);
        const int before = g.writesTo(HB_PIN);
        wd.tick(rc.state(), false, now);
        if (g.writesTo(HB_PIN) > before && g.level[HB_PIN]) {
            // A HIGH heartbeat edge — the pump must be commanded on right now.
            TEST_ASSERT_TRUE(vd.pumpIsOn());
            sawBeat = true;
        }
        now += 50;
    }
    TEST_ASSERT_TRUE(rc.isIdle());
    TEST_ASSERT_TRUE(sawBeat);                   // the window actually produced edges
    TEST_ASSERT_FALSE(g.level[HB_PIN]);          // parked LOW after the run
    TEST_ASSERT_TRUE(g.writesTo(HB_PIN) > baseline);
}

// ----------------------------------------------------------------------------
// FaultManager (§14, #5.3 software half): log ring + the resolved-condition
// clear gate that the button long-press and the Phase 4 endpoint share.
// ----------------------------------------------------------------------------

// The gate: a latched fault whose condition still holds refuses to clear; once
// the condition resolves, the same request succeeds and unlatches.
static void test_fm_clear_blocked_until_condition_resolves() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FaultManager fm(rc);

    // (UnexpectedFlow stands in — Watchdog no longer latches at all, DEC-023.)
    rc.raiseFault(Fault::UnexpectedFlow, 100);
    fm.tick(1750000000u, true);
    fm.setConditionActive(Fault::UnexpectedFlow, true);    // water still pulsing
    TEST_ASSERT_FALSE(fm.clearAllowed());
    TEST_ASSERT_FALSE(fm.requestClear());
    TEST_ASSERT_TRUE(rc.isFaulted());                // visible no-op, still latched

    fm.setConditionActive(Fault::UnexpectedFlow, false);   // flow decayed
    TEST_ASSERT_TRUE(fm.clearAllowed());
    TEST_ASSERT_TRUE(fm.requestClear());
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_TRUE(rc.isIdle());
}

// No latched fault -> nothing to clear; and a one-shot code nobody pushes a
// condition for (CalRange) clears freely.
static void test_fm_not_faulted_and_unconditioned_codes() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FaultManager fm(rc);

    TEST_ASSERT_FALSE(fm.clearAllowed());
    TEST_ASSERT_FALSE(fm.requestClear());

    rc.raiseFault(Fault::CalRange, 50);
    fm.tick(1750000000u, true);
    TEST_ASSERT_TRUE(fm.requestClear());             // no live condition — clears
    TEST_ASSERT_TRUE(rc.isIdle());
}

// The log: one entry per latch edge (not per tick), newest first, ring caps at
// LOG_SIZE keeping the most recent entries.
static void test_fm_log_edges_and_ring_wrap() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FaultManager fm(rc);

    const uint32_t E0 = 1750000000u;
    rc.raiseFault(Fault::NoFlow, 100);
    for (uint32_t t = 100; t < 600; t += 100) fm.tick(E0, true);   // held latch: one entry
    TEST_ASSERT_EQUAL_UINT8(1, fm.logCount());
    TEST_ASSERT_EQUAL(Fault::NoFlow, fm.logEntry(0).code);
    TEST_ASSERT_EQUAL_UINT32(E0, fm.logEntry(0).epoch);
    TEST_ASSERT_TRUE(fm.logEntry(0).clockWasValid);

    TEST_ASSERT_TRUE(fm.requestClear());
    rc.raiseFault(Fault::CalRange, 700);
    fm.tick(E0 + 7, true);
    TEST_ASSERT_EQUAL_UINT8(2, fm.logCount());
    TEST_ASSERT_EQUAL(Fault::CalRange, fm.logEntry(0).code);   // newest first
    TEST_ASSERT_EQUAL(Fault::NoFlow,   fm.logEntry(1).code);

    // Churn past LOG_SIZE (now 16): the ring keeps the newest LOG_SIZE.
    uint32_t e = E0 + 1000;
    for (int i = 0; i < 18; ++i) {
        TEST_ASSERT_TRUE(fm.requestClear());
        rc.raiseFault(Fault::UnexpectedFlow, 1000 + i);
        fm.tick(e, true);
        e += 100;
    }
    TEST_ASSERT_EQUAL_UINT8(FaultManager::LOG_SIZE, fm.logCount());
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow, fm.logEntry(0).code);
    TEST_ASSERT_EQUAL_UINT32(e - 100, fm.logEntry(0).epoch);    // the last raise
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow,
                      fm.logEntry(FaultManager::LOG_SIZE - 1).code);  // oldest kept
}

// #90: epoch stamping + the 2025 sanity guard + serialize/deserialize + NVS round-trip.
static void test_fm_epoch_guard_and_persist() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FaultManager fm(rc);

    fm.note(Fault::NoFlow,    1700000000u, true);   // 2023: pre-2025 → bit clear
    fm.note(Fault::ValveRest, 1750000000u, true);   // 2025 + synced → set
    fm.note(Fault::CalRange,  1750000000u, false);  // unsynced → clear
    TEST_ASSERT_EQUAL_UINT8(3, fm.logCount());
    TEST_ASSERT_FALSE(fm.logEntry(2).clockWasValid);              // oldest = NoFlow (pre-2025)
    TEST_ASSERT_EQUAL_UINT32(1700000000u, fm.logEntry(2).epoch);  // stored, just flagged
    TEST_ASSERT_TRUE (fm.logEntry(1).clockWasValid);             // ValveRest
    TEST_ASSERT_FALSE(fm.logEntry(0).clockWasValid);             // CalRange (unsynced)
    TEST_ASSERT_TRUE(fm.dirty());                                // note() dirties

    uint8_t blob[FaultManager::BLOB_BYTES];
    const uint16_t len = fm.serialize(blob, sizeof(blob));
    TEST_ASSERT_EQUAL_UINT16(3 * FaultManager::ENTRY_BYTES, len);
    FaultManager fm2(rc);
    fm2.deserialize(blob, len);
    TEST_ASSERT_EQUAL_UINT8(3, fm2.logCount());
    TEST_ASSERT_EQUAL(Fault::CalRange, fm2.logEntry(0).code);     // newest preserved
    TEST_ASSERT_EQUAL(Fault::NoFlow,   fm2.logEntry(2).code);     // oldest preserved
    TEST_ASSERT_EQUAL_UINT32(1750000000u, fm2.logEntry(1).epoch);
    TEST_ASSERT_TRUE(fm2.logEntry(1).clockWasValid);
    TEST_ASSERT_FALSE(fm2.dirty());                              // rehydrate is not a change

    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();
    FaultManager absent(rc);
    p.loadFaultLog(absent);
    TEST_ASSERT_EQUAL_UINT8(0, absent.logCount());               // absent key → empty
    p.saveFaultLog(fm);
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("faultlog"));
    Persistence p2(s, 3);
    p2.begin();
    FaultManager loaded(rc);
    p2.loadFaultLog(loaded);
    TEST_ASSERT_EQUAL_UINT8(3, loaded.logCount());
    TEST_ASSERT_EQUAL(Fault::CalRange, loaded.logEntry(0).code);
    TEST_ASSERT_FALSE(loaded.logEntry(0).clockWasValid);
    TEST_ASSERT_EQUAL_UINT32(1750000000u, loaded.logEntry(1).epoch);
}

// ----------------------------------------------------------------------------
// ValveRestMonitor (DEC-014 / #52): the auto-return self-test. Real RunController
// + ValveDriver drive the states; pulses are injected like the flow tests.
// ----------------------------------------------------------------------------

using tinkle::ValveRestMonitor;

namespace {

ValveRestMonitor::Config makeRestCfg() {
    ValveRestMonitor::Config c;
    c.windowMs      = 2000;     // short for tests; real seed 10 s (§15)
    c.maxRestPulses = 5;
    c.drainQuietMs     = 500;   // #124 drain gate, shortened to match
    c.drainQuietPulses = 2;
    c.drainCapMs       = 3000;
    return c;
}

} // namespace

// A healthy close: run flow stops with the pump, the rest window stays quiet,
// no flag — and the verdict is -1 on every single tick.
static void test_rest_clean_close_no_flag() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    ValveRestMonitor vr(makeRestCfg());
    uint32_t pulses = 0;

    RunRequest req; req.zoneIndex = 1; req.durationSec = 1;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));
    for (uint32_t now = 0; now < 15000; now += 50) {
        rc.tick(now);
        if (rc.state() == RunState::Running) pulses += 10;     // healthy run flow
        TEST_ASSERT_EQUAL_INT(-1, vr.tick(rc.state(), rc.lastRun().zoneIndex, pulses, now));
    }
    TEST_ASSERT_TRUE(rc.isIdle());
    TEST_ASSERT_FALSE(vr.zoneFlagged(1));
    TEST_ASSERT_EQUAL_UINT8(0, vr.flaggedMask());
}

// A stuck-open valve dribbles through the rest window: the just-closed zone gets
// flagged exactly once, other zones stay clean.
static void test_rest_stuck_valve_flags_zone() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    ValveRestMonitor vr(makeRestCfg());
    uint32_t pulses = 0;

    RunRequest req; req.zoneIndex = 1; req.durationSec = 1;
    TEST_ASSERT_TRUE(rc.requestRun(req, 0));
    int flaggedZone = -1;
    int verdicts    = 0;
    for (uint32_t now = 0; now < 15000; now += 50) {
        rc.tick(now);
        const RunState s = rc.state();
        if (s == RunState::Running) pulses += 10;
        if (s == RunState::Settle || s == RunState::Idle) pulses += 1;   // dribble
        const int v = vr.tick(s, rc.lastRun().zoneIndex, pulses, now);
        if (v >= 0) { flaggedZone = v; ++verdicts; }
    }
    TEST_ASSERT_EQUAL_INT(1, flaggedZone);
    TEST_ASSERT_EQUAL_INT(1, verdicts);                 // newly-flagged fires once
    TEST_ASSERT_TRUE(vr.zoneFlagged(1));
    TEST_ASSERT_FALSE(vr.zoneFlagged(0));
    TEST_ASSERT_EQUAL_UINT8(1u << 1, vr.flaggedMask());
}

// The flag self-heals: a later run on the same zone whose rest window stays
// quiet un-flags it (a re-seated valve clears its own flag).
static void test_rest_flag_self_heals_on_clean_close() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    ValveRestMonitor vr(makeRestCfg());
    uint32_t pulses = 0;
    uint32_t now = 0;

    // First run: dribble at rest -> flagged.
    RunRequest req; req.zoneIndex = 1; req.durationSec = 1;
    TEST_ASSERT_TRUE(rc.requestRun(req, now));
    for (; now < 15000; now += 50) {
        rc.tick(now);
        const RunState s = rc.state();
        if (s == RunState::Settle || s == RunState::Idle) pulses += 1;
        vr.tick(s, rc.lastRun().zoneIndex, pulses, now);
    }
    TEST_ASSERT_TRUE(vr.zoneFlagged(1));

    // Second run: quiet rest window -> the same zone un-flags.
    TEST_ASSERT_TRUE(rc.requestRun(req, now));
    for (uint32_t end = now + 15000; now < end; now += 50) {
        rc.tick(now);
        vr.tick(rc.state(), rc.lastRun().zoneIndex, pulses, now);   // no new pulses
    }
    TEST_ASSERT_TRUE(rc.isIdle());
    TEST_ASSERT_FALSE(vr.zoneFlagged(1));
}

// A queued run chaining SETTLE -> next aborts the check: the next run's own flow
// must never be read as rest flow. The dribble here would exceed the threshold
// if the window survived the chain — no flag may appear.
static void test_rest_chained_run_aborts_check() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    ValveRestMonitor vr(makeRestCfg());
    uint32_t pulses = 0;

    RunRequest first;  first.zoneIndex = 0;  first.durationSec = 1;
    RunRequest second; second.zoneIndex = 1; second.durationSec = 1;
    TEST_ASSERT_TRUE(rc.requestRun(first, 0));
    TEST_ASSERT_TRUE(rc.requestRun(second, 0));        // queues behind the first

    bool inSecondRun = false;
    for (uint32_t now = 0; now < 25000; now += 50) {
        rc.tick(now);
        const RunState s = rc.state();
        if (s == RunState::Running && rc.activeZone() == 1) inSecondRun = true;
        // Line noise from the first settle through the second run's staging —
        // exactly the span a surviving window would mismeasure.
        if (!inSecondRun && (s == RunState::Settle || s == RunState::PrepDiverter ||
                             s == RunState::OpenZone || s == RunState::StartPump))
            pulses += 1;
        TEST_ASSERT_EQUAL_INT(-1, vr.tick(s, rc.lastRun().zoneIndex, pulses, now));
    }
    TEST_ASSERT_TRUE(rc.isIdle());
    TEST_ASSERT_EQUAL_UINT8(0, vr.flaggedMask());      // zone 1's clean final window
}

// Threshold boundary, synthetic states: exactly maxRestPulses is rest-quiet
// (no flag, window completes clean); one more flags. The #124 drain gate sits in
// front of the window, so each SETTLE entry pays a quiet drainQuietMs first.
static void test_rest_threshold_boundary() {
    ValveRestMonitor vr(makeRestCfg());
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 2, 100, 0));      // drain wait opens
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 2, 100, 500));    // quiet -> window opens
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   2, 105, 1000));   // == threshold
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   2, 105, 2499));
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   2, 105, 2500));   // closes clean
    TEST_ASSERT_FALSE(vr.zoneFlagged(2));

    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Running, 2, 200, 3000));  // next run
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle,  2, 200, 4000));  // drain wait
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle,  2, 200, 4500));  // quiet -> window opens
    TEST_ASSERT_EQUAL_INT( 2, vr.tick(RunState::Idle,    2, 206, 5000));  // threshold + 1
    TEST_ASSERT_TRUE(vr.zoneFlagged(2));
}

// The #124 repro for DEC-014: heavy trailing draindown right after close travel —
// pulses that would have blown REST_MAX_PULSES land during the drain wait, then the
// meter quiets and the real window runs clean. No flag: a healthy-but-slow-seating
// valve is not a stuck valve.
static void test_rest_draindown_then_quiet_no_flag() {
    ValveRestMonitor vr(makeRestCfg());
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 1, 100, 0));      // drain wait opens
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 1, 120, 250));    // +20 trailing (old code: flag)
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   1, 135, 500));    // decaying
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   1, 140, 750));
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   1, 141, 1250));   // quiet 500 ms -> window opens
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Idle,   1, 142, 3250));   // window closes clean
    TEST_ASSERT_FALSE(vr.zoneFlagged(1));
    TEST_ASSERT_EQUAL_UINT8(0, vr.flaggedMask());
}

// Flow that never quiets after close travel is the DEC-014 failure itself: the
// drain gate caps out and flags the zone directly — still exactly once.
static void test_rest_never_quiet_flags_via_cap() {
    ValveRestMonitor vr(makeRestCfg());
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 2, 100, 0));
    int flagged = -1;
    int verdicts = 0;
    uint32_t pulses = 100;
    for (uint32_t now = 250; now <= 4000; now += 250) {
        pulses += 10;                                                     // never decays
        const int v = vr.tick(RunState::Idle, 2, pulses, now);
        if (v >= 0) { flagged = v; ++verdicts; }
    }
    TEST_ASSERT_EQUAL_INT(2, flagged);
    TEST_ASSERT_EQUAL_INT(1, verdicts);
    TEST_ASSERT_TRUE(vr.zoneFlagged(2));
}

// A fault latching mid-window aborts the check — even with a dribble already
// past the threshold's worth of pulses pending, the Fault state must produce
// no verdict and no flag (the fault owns the story; the window measured a
// fault transient, not rest flow). Also locks the raiseFault firewall: a
// stray raiseFault(ValveRest) must be a no-op, never a latch.
static void test_rest_fault_aborts_window_and_valverest_cannot_latch() {
    ValveRestMonitor vr(makeRestCfg());
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Settle, 1, 100, 0));     // window opens
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Fault,  1, 120, 500));   // dribble + fault
    TEST_ASSERT_FALSE(vr.zoneFlagged(1));
    TEST_ASSERT_EQUAL_INT(-1, vr.tick(RunState::Fault,  1, 200, 1000));  // stays aborted
    TEST_ASSERT_EQUAL_UINT8(0, vr.flaggedMask());

    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    rc.raiseFault(Fault::ValveRest, 100);            // DEC-014: log-only, must not latch
    TEST_ASSERT_FALSE(rc.isFaulted());
    TEST_ASSERT_TRUE(rc.isIdle());
}

// ----------------------------------------------------------------------------
// Api (§10, Unit C / #55-#57): endpoint policy against the REAL wire shapes —
// ArduinoJson on the host, full dependency rig, no parallel model.
// ----------------------------------------------------------------------------

using tinkle::Api;
using tinkle::FlowFaultDetector;
using tinkle::FlowMonitor;
using tinkle::Scheduler;
using tinkle::ScheduleEntry;

namespace {

struct ApiRig {
    ValveDriver           vd;
    RunController         rc;
    FakeKvStore           kv;
    Persistence           store;
    FlowMonitor           flow;
    FlowFaultDetector     flowFault;
    FaultManager          fm;
    CalibrationController cal;
    ValveRestMonitor      rest;
    FakeWallClock         wallSrc;
    tinkle::Clock         clk;
    Scheduler             sched;
    Api                   api;

    ApiRig()
        : vd(g, cfg), rc(vd, makeRunCfg()), store(kv, 2),
          flow(Persistence::DEFAULT_PULSES_PER_GALLON),
          flowFault(FlowFaultDetector::Config{}),
          fm(rc), cal(rc, store, flow, calCfg()), rest(makeRestCfg()),
          clk(wallSrc), sched(rc, clk),
          api(Api::Deps{rc, sched, store, fm, cal, flow, flowFault, rest, clk}) {
        rc.begin(0);
        store.begin();
    }

    // Mirror the main loop (controller + calibration), stepping the clock.
    template <class Pred>
    uint32_t pump(uint32_t now, uint32_t stepMs, uint32_t budgetMs,
                  const uint32_t& pulses, Pred pred) {
        uint32_t spent = 0;
        while (!pred() && spent <= budgetMs) {
            rc.tick(now);
            cal.tick(rc.state(), pulses, now);
            now += stepMs;
            spent += stepMs;
        }
        return now;
    }
};

// Parse a JSON literal into a document (test helper; asserts on parse failure).
JsonDocument parseJson(const char* text) {
    JsonDocument doc;
    TEST_ASSERT_TRUE_MESSAGE(deserializeJson(doc, text) == DeserializationError::Ok,
                             "test JSON literal failed to parse");
    return doc;
}

} // namespace

// GET /api/status at boot: idle, no fault, defaults — the §10 shape.
static void test_api_status_shape() {
    ApiRig r;
    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.getStatus(out, 0));
    TEST_ASSERT_EQUAL_STRING("idle", out["state"]);
    TEST_ASSERT_EQUAL_INT(-1, out["activeZone"].as<int>());
    TEST_ASSERT_FALSE(out["activeFert"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(0, out["remainingSec"].as<uint32_t>());
    TEST_ASSERT_EQUAL_FLOAT(Persistence::DEFAULT_PULSES_PER_GALLON, out["flow"]["k"]);
    TEST_ASSERT_FALSE(out["flow"]["overrideActive"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("none", out["fault"]["active"]);
    TEST_ASSERT_FALSE(out["fault"]["clearAllowed"].as<bool>());
    TEST_ASSERT_EQUAL_INT(0, out["fault"]["log"].size());
    TEST_ASSERT_EQUAL_UINT8(0, out["valveRestFlags"].as<uint8_t>());
    TEST_ASSERT_FALSE(out["clock"]["valid"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("idle", out["cal"]["phase"]);
}

// POST /api/run: 400 missing field, 422 out of range, 200 starts with the stored
// per-zone default, queues behind an active run.
static void test_api_run_validation_and_start() {
    ApiRig r;
    JsonDocument out;

    JsonDocument body = parseJson("{}");
    TEST_ASSERT_EQUAL_INT(400, r.api.postRun(body.as<JsonVariantConst>(), out, 0));

    out.clear(); body = parseJson("{\"zoneIndex\":5}");
    TEST_ASSERT_EQUAL_INT(422, r.api.postRun(body.as<JsonVariantConst>(), out, 0));

    out.clear(); body = parseJson("{\"zoneIndex\":0,\"durationSec\":5}");   // < RUN_MIN_SEC
    TEST_ASSERT_EQUAL_INT(422, r.api.postRun(body.as<JsonVariantConst>(), out, 0));

    out.clear(); body = parseJson("{\"zoneIndex\":0,\"durationSec\":\"abc\"}");
    TEST_ASSERT_EQUAL_INT(400, r.api.postRun(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_TRUE(r.rc.isIdle());                       // nothing slipped through

    out.clear(); body = parseJson("{\"zoneIndex\":0}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postRun(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, out["durationSec"].as<uint32_t>());
    TEST_ASSERT_FALSE(out["queued"].as<bool>());
    TEST_ASSERT_FALSE(r.rc.isIdle());

    out.clear(); body = parseJson("{\"zoneIndex\":1,\"durationSec\":60,\"fertigate\":true}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postRun(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_TRUE(out["queued"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(1, r.rc.queueDepth());
}

// The §10 FAULT gate: mutating endpoints 409 in FAULT; /stop and /fault/clear are
// exempt, and the clear obeys the FaultManager resolved-condition gate over HTTP.
static void test_api_fault_gate_and_exemptions() {
    ApiRig r;
    JsonDocument out;
    // (UnexpectedFlow stands in — Watchdog no longer latches at all, DEC-023.)
    r.rc.raiseFault(Fault::UnexpectedFlow, 100);
    r.fm.tick(1750000000u, true);
    r.fm.setConditionActive(Fault::UnexpectedFlow, true);  // water still pulsing

    JsonDocument body = parseJson("{\"zoneIndex\":0}");
    TEST_ASSERT_EQUAL_INT(409, r.api.postRun(body.as<JsonVariantConst>(), out, 200));
    out.clear();
    body = parseJson("{\"entries\":[]}");
    TEST_ASSERT_EQUAL_INT(409, r.api.postSchedule(body.as<JsonVariantConst>(), out, 200));
    out.clear();
    body = parseJson("{\"zoneIndex\":0}");
    TEST_ASSERT_EQUAL_INT(409, r.api.postCalStart(body.as<JsonVariantConst>(), out, 0, 200));

    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.postStop(out, 200));  // exempt (harmless no-op)

    out.clear();
    TEST_ASSERT_EQUAL_INT(409, r.api.postFaultClear(out, 300));   // condition holds
    TEST_ASSERT_FALSE(out["cleared"].as<bool>());
    TEST_ASSERT_TRUE(r.rc.isFaulted());

    r.fm.setConditionActive(Fault::UnexpectedFlow, false); // flow decayed
    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.postFaultClear(out, 400));
    TEST_ASSERT_TRUE(out["cleared"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("unexpectedFlow", out["was"]);
    TEST_ASSERT_TRUE(r.rc.isIdle());
}

// POST /api/schedule: atomic replace, NVS save-on-edit, GET roundtrip, and a bad
// entry rejects the whole post leaving the live schedule untouched.
static void test_api_schedule_roundtrip_persist_atomic() {
    ApiRig r;
    JsonDocument out;
    JsonDocument body = parseJson(
        "{\"entries\":["
        "{\"zoneIndex\":0,\"hour\":6,\"minute\":30,\"durationSec\":600,\"daysMask\":127,\"enabled\":true},"
        "{\"zoneIndex\":1,\"hour\":18,\"minute\":0,\"durationSec\":300,\"daysMask\":42,\"fertOverride\":2,\"enabled\":true}"
        "]}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postSchedule(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_UINT8(2, r.sched.count());
    TEST_ASSERT_TRUE(r.kv.bytes.count("sched") == 1);      // §13 save-on-edit hit NVS

    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.getSchedule(out));
    TEST_ASSERT_EQUAL_INT(2, out["entries"].size());
    TEST_ASSERT_EQUAL_INT(18,  out["entries"][1]["hour"].as<int>());
    TEST_ASSERT_EQUAL_INT(42,  out["entries"][1]["daysMask"].as<int>());
    TEST_ASSERT_EQUAL_INT(2,   out["entries"][1]["fertOverride"].as<int>());

    // Atomicity: hour 24 rejects the whole post; the live schedule stays at 2.
    out.clear();
    body = parseJson("{\"entries\":[{\"zoneIndex\":0,\"hour\":24,\"minute\":0,"
                     "\"durationSec\":600,\"daysMask\":127,\"enabled\":true}]}");
    TEST_ASSERT_EQUAL_INT(422, r.api.postSchedule(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_UINT8(2, r.sched.count());

    // Reboot path: a fresh Persistence + Scheduler over the same store reloads both.
    Persistence store2(r.kv, 2);
    store2.begin();
    ScheduleEntry loaded[Scheduler::MAX_ENTRIES];
    const uint8_t n = store2.loadScheduleEntries(loaded, Scheduler::MAX_ENTRIES);
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_UINT8(1,   loaded[1].zoneIndex);
    TEST_ASSERT_EQUAL_UINT16(300, loaded[1].durationSec);
    TEST_ASSERT_EQUAL(tinkle::FertOverride::Off, loaded[1].fertOverride);
    TEST_ASSERT_TRUE(loaded[1].enabled);
}

// GET/POST /api/settings: defaults out, partial update in, ranges enforced before
// anything applies, swMax live-caps the active run, the passphrase never echoes.
static void test_api_settings_get_post() {
    ApiRig r;
    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.getSettings(out));
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_RUN_SEC, out["zoneDefaults"][0].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(Persistence::DEFAULT_SW_MAX_SEC, out["swMaxRuntimeSec"].as<uint32_t>());
    TEST_ASSERT_FALSE(out["wifi"]["configured"].as<bool>());

    out.clear();
    JsonDocument body = parseJson(
        "{\"swMaxRuntimeSec\":900,\"pulsesPerGallon\":500,\"zoneDefaults\":[300,400],"
        "\"wifi\":{\"ssid\":\"farm-mesh\",\"pass\":\"hunter22\"}}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postSettings(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_UINT32(900, r.store.swMaxRuntimeSec());
    TEST_ASSERT_EQUAL_FLOAT(500.0f, r.flow.k());
    TEST_ASSERT_EQUAL_UINT32(300, r.store.zoneDefaultSec(0));
    TEST_ASSERT_EQUAL_UINT32(400, r.store.zoneDefaultSec(1));
    TEST_ASSERT_EQUAL_STRING("farm-mesh", r.store.wifiSsid());
    TEST_ASSERT_EQUAL_STRING("hunter22",  r.store.wifiPass());
    TEST_ASSERT_EQUAL_STRING("farm-mesh", out["wifi"]["ssid"]);
    TEST_ASSERT_TRUE(out["wifi"]["pass"].isNull());        // write-only, never echoed

    // Out-of-range rejects before anything applies (atomic).
    out.clear();
    body = parseJson("{\"swMaxRuntimeSec\":5000,\"pulsesPerGallon\":600}");
    TEST_ASSERT_EQUAL_INT(422, r.api.postSettings(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_UINT32(900, r.store.swMaxRuntimeSec());
    TEST_ASSERT_EQUAL_FLOAT(500.0f, r.flow.k());           // the valid field didn't slip in

    // Type leniency is rejected loudly: a non-bool override (the DEC-015 safety
    // knob) and a non-string wifi.pass (would coerce to an empty passphrase) 400.
    out.clear();
    body = parseJson("{\"flowOverride\":1}");
    TEST_ASSERT_EQUAL_INT(400, r.api.postSettings(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_FALSE(r.store.flowOverride());
    out.clear();
    body = parseJson("{\"wifi\":{\"ssid\":\"x\",\"pass\":12345}}");
    TEST_ASSERT_EQUAL_INT(400, r.api.postSettings(body.as<JsonVariantConst>(), out, 0));
    TEST_ASSERT_EQUAL_STRING("farm-mesh", r.store.wifiSsid());   // untouched

    // swMax was applied LIVE to RunController: a 7200 s request caps at 900.
    JsonDocument runOut;
    body = parseJson("{\"zoneIndex\":0,\"durationSec\":7200}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postRun(body.as<JsonVariantConst>(), runOut, 1000));
    const uint32_t pulses = 0;
    uint32_t now = r.pump(1000, 5, 8000, pulses, [&]{ return r.rc.state() == RunState::Running; });
    TEST_ASSERT_TRUE(r.rc.remainingSec(now) <= 900);
}

// DEC-024 Distributed Watering config API: GET/POST roundtrip, server-computed plan,
// validation backstop, and the fault gate.
static void test_api_distributed_roundtrip() {
    ApiRig r;
    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.getDistributed(out));
    TEST_ASSERT_FALSE(out["enabled"].as<bool>());
    TEST_ASSERT_FALSE(out["active"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(3, out["zoneCount"].as<uint8_t>());   // sched default MAX_ZONES
    TEST_ASSERT_EQUAL_UINT16(7, out["floorMin"].as<uint16_t>());

    JsonDocument body = parseJson(
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":930,"
        "\"perZoneMin\":32,\"fertCount\":1}");
    JsonDocument o2;
    TEST_ASSERT_EQUAL_INT(200, r.api.postDistributed(body.as<JsonVariantConst>(), o2, 0));
    TEST_ASSERT_TRUE(o2["active"].as<bool>());
    TEST_ASSERT_TRUE(r.store.distributed().enabled);             // persisted
    TEST_ASSERT_TRUE(r.sched.distributed().enabled);             // live

    JsonDocument o3;
    TEST_ASSERT_EQUAL_INT(200, r.api.getDistributed(o3));
    TEST_ASSERT_EQUAL_UINT16(540, o3["windowStartMin"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(32,  o3["perZoneMin"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT8(1,    o3["fertCount"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT8(4,    o3["plan"]["cycles"].as<uint8_t>());       // matches firmware
    TEST_ASSERT_EQUAL_UINT16(480, o3["plan"]["runLenSec"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(540, o3["plan"]["cycleStartMin"][0].as<uint16_t>());
}

static void test_api_distributed_rejects_invalid() {
    ApiRig r;
    JsonDocument o1;
    TEST_ASSERT_EQUAL_INT(422, r.api.postDistributed(parseJson(   // over-subscribed window
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":570,\"perZoneMin\":42,\"fertCount\":0}")
        .as<JsonVariantConst>(), o1, 0));
    JsonDocument o2;
    TEST_ASSERT_EQUAL_INT(422, r.api.postDistributed(parseJson(   // below the 7-min floor
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":930,\"perZoneMin\":5,\"fertCount\":0}")
        .as<JsonVariantConst>(), o2, 0));
    JsonDocument o3;
    TEST_ASSERT_EQUAL_INT(422, r.api.postDistributed(parseJson(   // fertCount past the cap
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":930,\"perZoneMin\":32,\"fertCount\":9}")
        .as<JsonVariantConst>(), o3, 0));
    TEST_ASSERT_FALSE(r.store.distributed().enabled);            // nothing persisted
}

static void test_api_distributed_disable_and_fault_gate() {
    ApiRig r;
    JsonDocument o1;
    r.api.postDistributed(parseJson(
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":930,\"perZoneMin\":32,\"fertCount\":0}")
        .as<JsonVariantConst>(), o1, 0);
    TEST_ASSERT_TRUE(r.sched.distributed().enabled);

    JsonDocument o2;                                             // disable -> schedule governs
    TEST_ASSERT_EQUAL_INT(200, r.api.postDistributed(
        parseJson("{\"enabled\":false}").as<JsonVariantConst>(), o2, 0));
    TEST_ASSERT_FALSE(o2["active"].as<bool>());
    TEST_ASSERT_FALSE(r.sched.distributed().enabled);
    TEST_ASSERT_EQUAL_UINT16(32, r.sched.distributed().perZoneMin);   // disable keeps the plan

    r.rc.raiseFault(Fault::NoFlow, 0);                           // faulted -> 409, no edit
    JsonDocument o3;
    TEST_ASSERT_EQUAL_INT(409, r.api.postDistributed(parseJson(
        "{\"enabled\":true,\"windowStartMin\":540,\"windowEndMin\":930,\"perZoneMin\":32,\"fertCount\":0}")
        .as<JsonVariantConst>(), o3, 0));
}

// DEC-015 (#57): enabling the override mutes flow verdicts, clears a latched flow
// fault even while water still pulses (the recovery path), and persists.
static void test_api_flow_override_dec015() {
    ApiRig r;
    JsonDocument out;

    r.rc.raiseFault(Fault::UnexpectedFlow, 100);
    r.fm.tick(1750000000u, true);
    r.fm.setConditionActive(Fault::UnexpectedFlow, true);  // water still pulsing
    TEST_ASSERT_EQUAL_INT(409, r.api.postFaultClear(out, 150));

    out.clear();
    JsonDocument body = parseJson("{\"flowOverride\":true}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postSettings(body.as<JsonVariantConst>(), out, 200));
    TEST_ASSERT_TRUE(out["flowOverride"].as<bool>());
    TEST_ASSERT_FALSE(r.rc.isFaulted());                   // cleared by the enable
    TEST_ASSERT_TRUE(r.rc.isIdle());
    TEST_ASSERT_TRUE(r.store.flowOverride());              // persisted
    TEST_ASSERT_TRUE(r.flowFault.muted());                 // detector muted

    // A NON-flow fault is untouched by the override path (CalRange stands in —
    // Watchdog no longer latches at all, DEC-023).
    r.rc.raiseFault(Fault::CalRange, 300);
    r.fm.tick(1750000300u, true);
    out.clear();
    body = parseJson("{\"flowOverride\":true}");           // re-post, already on
    TEST_ASSERT_EQUAL_INT(200, r.api.postSettings(body.as<JsonVariantConst>(), out, 350));
    TEST_ASSERT_TRUE(r.rc.isFaulted());                    // non-flow fault survives
    TEST_ASSERT_EQUAL(Fault::CalRange, r.rc.activeFault());
}

// The detector-side mute (DEC-015): verdicts go quiet, tracking does not — the
// idle window keeps sliding while muted, so un-muting never judges the muted span.
static void test_flow_detector_mute() {
    FlowFaultDetector::Config c;
    c.graceMs = 1000; c.minRunningGPM = 0.1f; c.idleFaultPulses = 5; c.idleWindowMs = 1000;
    c.drainQuietMs = 500; c.drainQuietPulses = 2; c.drainCapMs = 3000;   // #124 gate, test-short

    FlowFaultDetector unmuted(c);
    unmuted.begin(0, 0);
    unmuted.update(RunState::Running, 0.0f, 0, 100);       // RUNNING edge: grace starts
    TEST_ASSERT_EQUAL(Fault::NoFlow, unmuted.update(RunState::Running, 0.0f, 0, 1200));

    FlowFaultDetector muted(c);
    muted.begin(0, 0);
    muted.setMuted(true);
    muted.update(RunState::Running, 0.0f, 0, 100);
    TEST_ASSERT_EQUAL(Fault::None, muted.update(RunState::Running, 0.0f, 0, 1200));

    // Idle: 50 pulses land while muted -> no verdict, but tracking continues (the
    // #124 drain gate + the window keep sliding); un-mute -> the next window is
    // judged fresh, not the muted backlog — and the check is genuinely live.
    muted.update(RunState::Idle, 0.0f, 100, 1300);         // IDLE edge: drain gate opens
    TEST_ASSERT_EQUAL(Fault::None, muted.update(RunState::Idle, 0.0f, 150, 2400));   // muted burst
    muted.setMuted(false);
    TEST_ASSERT_EQUAL(Fault::None, muted.update(RunState::Idle, 0.0f, 150, 3500));   // quiet -> armed
    TEST_ASSERT_EQUAL(Fault::None, muted.update(RunState::Idle, 0.0f, 152, 4500));   // fresh window, clean
    TEST_ASSERT_EQUAL(Fault::UnexpectedFlow,
                      muted.update(RunState::Idle, 0.0f, 200, 5500));                // live post-unmute
}

// OTA gate (#126): begin only from IDLE or FAULT; while active, NOTHING may start
// a run — the API's postRun and the raw IRunSink seam the scheduler uses both
// refuse (the request-time gate alone can't stop a scheduled run coming due
// mid-flash; the inhibit in requestRun is the safety core). A failed upload
// lifts the inhibit via otaAbort().
static void test_api_ota_gate_and_run_inhibit() {
    ApiRig r;
    JsonDocument out;

    // Idle -> accepted; inhibit set.
    TEST_ASSERT_EQUAL_INT(200, r.api.postOtaBegin(out, 0));
    TEST_ASSERT_TRUE(r.rc.otaActive());

    // Second begin while one is in flight -> 409.
    out.clear();
    TEST_ASSERT_EQUAL_INT(409, r.api.postOtaBegin(out, 10));

    // Manual run (API) and the scheduler's IRunSink path both refused mid-flash.
    out.clear();
    JsonDocument body = parseJson("{\"zoneIndex\":0,\"durationSec\":60}");
    TEST_ASSERT_EQUAL_INT(409, r.api.postRun(body.as<JsonVariantConst>(), out, 20));
    RunRequest req; req.zoneIndex = 0; req.durationSec = 60;
    TEST_ASSERT_FALSE(r.rc.requestRun(req, 30));
    TEST_ASSERT_TRUE(r.rc.isIdle());                     // nothing moved

    // Failed/aborted upload -> otaAbort lifts the inhibit; runs flow again.
    r.api.otaAbort();
    TEST_ASSERT_FALSE(r.rc.otaActive());
    TEST_ASSERT_TRUE(r.rc.requestRun(req, 40));

    // And with that run now active, a begin is refused — no reflash mid-run.
    out.clear();
    TEST_ASSERT_EQUAL_INT(409, r.api.postOtaBegin(out, 50));
    TEST_ASSERT_FALSE(r.rc.otaActive());
}

// OTA from FAULT is allowed — a latched fault is pump-off and queue-unwound (the
// safest possible time to reflash), and OTA-while-faulted is a legitimate recovery
// path: the latch is RAM-only and re-derives from live conditions after reboot.
static void test_api_ota_allowed_while_faulted() {
    ApiRig r;
    r.rc.raiseFault(Fault::NoFlow, 0);
    TEST_ASSERT_TRUE(r.rc.isFaulted());
    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.postOtaBegin(out, 10));
    TEST_ASSERT_TRUE(r.rc.otaActive());
}

// Calibration endpoints: lifecycle mapping (409 not-calibrating / busy), the happy
// path computing K over a driven run, and Rejected -> 422 + FAULT_CAL_RANGE.
static void test_api_cal_endpoints() {
    ApiRig r;
    JsonDocument out;

    JsonDocument body = parseJson("{\"measuredGallons\":2.0}");
    TEST_ASSERT_EQUAL_INT(409, r.api.postCalFinish(body.as<JsonVariantConst>(), out, 0, 0));

    out.clear(); body = parseJson("{\"zoneIndex\":9}");
    TEST_ASSERT_EQUAL_INT(422, r.api.postCalStart(body.as<JsonVariantConst>(), out, 0, 0));

    uint32_t pulses = 0;
    out.clear(); body = parseJson("{\"zoneIndex\":0}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postCalStart(body.as<JsonVariantConst>(), out, pulses, 0));
    TEST_ASSERT_TRUE(r.cal.active());

    out.clear();                                            // double-start refused
    TEST_ASSERT_EQUAL_INT(409, r.api.postCalStart(body.as<JsonVariantConst>(), out, pulses, 10));

    uint32_t now = r.pump(10, 5, 8000, pulses, [&]{ return r.rc.state() == RunState::Running; });
    pulses += 900;
    now = r.pump(now, 100, 90000, pulses,
                 [&]{ return r.cal.phase() == CalibrationController::Phase::Awaiting; });

    out.clear(); body = parseJson("{\"measuredGallons\":2.0}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postCalFinish(body.as<JsonVariantConst>(), out, pulses, now));
    TEST_ASSERT_EQUAL_FLOAT(450.0f, out["k"]);
    TEST_ASSERT_EQUAL_FLOAT(450.0f, r.flow.k());

    // Parse-sane but below the controller's minGallons -> Rejected -> 422 + latch.
    // (Awaiting freezes at SETTLE entry, so let the controller reach IDLE first —
    // a calibration never starts over a still-settling run.)
    now = r.pump(now, 100, 70000, pulses, [&]{ return r.rc.isIdle(); });
    body = parseJson("{\"zoneIndex\":0}");
    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.postCalStart(body.as<JsonVariantConst>(), out, pulses, now));
    now = r.pump(now, 5, 8000, pulses, [&]{ return r.rc.state() == RunState::Running; });
    pulses += 500;
    out.clear(); body = parseJson("{\"measuredGallons\":0.1}");
    TEST_ASSERT_EQUAL_INT(422, r.api.postCalFinish(body.as<JsonVariantConst>(), out, pulses, now));
    TEST_ASSERT_TRUE(r.rc.isFaulted());
    TEST_ASSERT_EQUAL(Fault::CalRange, r.rc.activeFault());
}

// /api/stop voids an active calibration (operator bailed: no K, no CalRange) and
// unwinds the run.
static void test_api_stop_voids_calibration() {
    ApiRig r;
    JsonDocument out;
    uint32_t pulses = 0;
    JsonDocument body = parseJson("{\"zoneIndex\":0}");
    TEST_ASSERT_EQUAL_INT(200, r.api.postCalStart(body.as<JsonVariantConst>(), out, pulses, 0));
    uint32_t now = r.pump(0, 5, 8000, pulses, [&]{ return r.rc.state() == RunState::Running; });

    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.postStop(out, now));
    TEST_ASSERT_FALSE(r.cal.active());
    now = r.pump(now, 100, 70000, pulses, [&]{ return r.rc.isIdle(); });
    TEST_ASSERT_TRUE(r.rc.isIdle());
    TEST_ASSERT_FALSE(r.rc.isFaulted());
    TEST_ASSERT_EQUAL_INT(0, r.kv.writesTo("k_ppg"));      // no K written
}

// Schedule pack/unpack: explicit little-endian roundtrip + garbage fert clamps.
static void test_sched_pack_unpack_roundtrip() {
    ScheduleEntry e;
    e.id = 7; e.zoneIndex = 2; e.hour = 23; e.minute = 59;
    e.durationSec = 0x1234; e.daysMask = 0x55;
    e.fertOverride = tinkle::FertOverride::On; e.enabled = true;

    uint8_t buf[tinkle::SCHED_ENTRY_BYTES];
    tinkle::packScheduleEntry(e, buf);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[4]);                 // little-endian low byte
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[5]);

    ScheduleEntry d = tinkle::unpackScheduleEntry(buf);
    TEST_ASSERT_EQUAL_UINT8(7, d.id);
    TEST_ASSERT_EQUAL_UINT8(2, d.zoneIndex);
    TEST_ASSERT_EQUAL_UINT16(0x1234, d.durationSec);
    TEST_ASSERT_EQUAL(tinkle::FertOverride::On, d.fertOverride);
    TEST_ASSERT_TRUE(d.enabled);

    buf[7] = 9;                                            // corrupt fert byte
    TEST_ASSERT_EQUAL(tinkle::FertOverride::Auto, tinkle::unpackScheduleEntry(buf).fertOverride);
}

// Persistence: flow-override + WiFi creds survive a "reboot" (fresh mirror, same store).
static void test_persist_override_and_creds_roundtrip() {
    FakeKvStore s;
    {
        Persistence p(s, 2);
        p.begin();
        TEST_ASSERT_FALSE(p.flowOverride());               // DEC-015 default: checks ON
        TEST_ASSERT_EQUAL_STRING("", p.wifiSsid());
        p.setFlowOverride(true);
        p.setWifiCreds("tunnel-net", "drip4days");
        p.setWifiCreds("tunnel-net", "drip4days");          // write-on-change: no second write
    }
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("wifi_ssid"));
    Persistence p2(s, 2);
    p2.begin();
    TEST_ASSERT_TRUE(p2.flowOverride());
    TEST_ASSERT_EQUAL_STRING("tunnel-net", p2.wifiSsid());
    TEST_ASSERT_EQUAL_STRING("drip4days",  p2.wifiPass());
}

// FaultManager::note() is log-only: the entry lands in the ring, nothing
// latches, and runs are still accepted.
static void test_fm_note_is_nonlatching() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    FaultManager fm(rc);

    fm.note(Fault::ValveRest, 1750000000u, true);
    TEST_ASSERT_EQUAL_UINT8(1, fm.logCount());
    TEST_ASSERT_EQUAL(Fault::ValveRest, fm.logEntry(0).code);
    TEST_ASSERT_EQUAL_UINT32(1750000000u, fm.logEntry(0).epoch);
    TEST_ASSERT_FALSE(rc.isFaulted());

    RunRequest req; req.zoneIndex = 0; req.durationSec = 1;
    TEST_ASSERT_TRUE(rc.requestRun(req, 200));          // not blocked by the note
}

// --- RunLog run-history ring (DEC-018, #69) --------------------------------------

// Drive one complete run, injecting the start epoch + per-run volume the way main does
// (noteRunStart on the RUNNING edge, noteRunVolume while pumping). Returns the clock reached.
static uint32_t runOnce(RunController& rc, uint32_t now, uint8_t zone, uint32_t durSec,
                        bool fert, uint32_t epoch, bool clockValid, uint16_t centigal) {
    RunRequest req; req.zoneIndex = zone; req.durationSec = durSec; req.fertigate = fert;
    rc.requestRun(req, now);
    now = pump(rc, now, 5, 12000, [&]{ return rc.state() == RunState::Running; });
    rc.noteRunStart(epoch, clockValid);
    rc.noteRunVolume(centigal);
    now = pump(rc, now, 5, 12000, [&]{ return rc.state() == RunState::Idle; });
    return now;
}

// Packed 11-byte record survives a round-trip, every flag bit independent (fert | result |
// clockWasValid) and the u16/u32 fields little-endian intact.
static void test_runlog_pack_unpack_roundtrip() {
    using tinkle::packRunEntry; using tinkle::unpackRunEntry;
    TEST_ASSERT_EQUAL_UINT16(11, tinkle::RUNLOG_ENTRY_BYTES);

    RunEntry e;
    e.startEpoch = 0xDEADBEEFu; e.zoneIndex = 5; e.durationSec = 0x1234;
    e.centigallons = 0xABCD; e.fertigate = true; e.result = RunResult::Stopped;
    e.clockWasValid = true;  e.faultCode = (uint8_t)Fault::Watchdog;

    uint8_t buf[tinkle::RUNLOG_ENTRY_BYTES];
    packRunEntry(e, buf);
    RunEntry r = unpackRunEntry(buf);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, r.startEpoch);
    TEST_ASSERT_EQUAL_UINT8(5, r.zoneIndex);
    TEST_ASSERT_EQUAL_UINT16(0x1234, r.durationSec);
    TEST_ASSERT_EQUAL_UINT16(0xABCD, r.centigallons);
    TEST_ASSERT_TRUE(r.fertigate);
    TEST_ASSERT_EQUAL(RunResult::Stopped, r.result);
    TEST_ASSERT_TRUE(r.clockWasValid);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Fault::Watchdog, r.faultCode);

    // Flip every flag the other way — result Faulted (3) exercises both result bits.
    e.fertigate = false; e.clockWasValid = false; e.result = RunResult::Faulted;
    packRunEntry(e, buf);
    r = unpackRunEntry(buf);
    TEST_ASSERT_FALSE(r.fertigate);
    TEST_ASSERT_FALSE(r.clockWasValid);
    TEST_ASSERT_EQUAL(RunResult::Faulted, r.result);
}

// The ring saturates at RUNLOG_DEPTH, overwriting the oldest; head() is the newest, at(0)
// == head, at(count-1) the oldest retained, and an empty ring's head defaults to None.
static void test_runlog_ring_wrap() {
    RunLog log;
    TEST_ASSERT_TRUE(log.empty());
    TEST_ASSERT_EQUAL(RunResult::None, log.head().result);

    const int total = tinkle::RUNLOG_DEPTH + 5;
    for (int i = 0; i < total; ++i) {
        RunEntry e; e.zoneIndex = (uint8_t)i; e.durationSec = (uint16_t)(i * 10);
        log.push(e);
    }
    TEST_ASSERT_EQUAL_UINT8(tinkle::RUNLOG_DEPTH, log.count());      // saturated, not grown
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(total - 1), log.head().zoneIndex);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(total - 1), log.at(0).zoneIndex);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(total - tinkle::RUNLOG_DEPTH),
                            log.at(tinkle::RUNLOG_DEPTH - 1).zoneIndex);
    TEST_ASSERT_EQUAL(RunResult::None, log.at(tinkle::RUNLOG_DEPTH).result);   // OOB -> default
}

// serialize/deserialize preserve order + head; a wrapped ring round-trips to exactly DEPTH
// entries; a torn (non-multiple) blob truncates to whole entries; an empty blob -> empty ring.
static void test_runlog_serialize_roundtrip() {
    RunLog a;
    for (int i = 0; i < 3; ++i) {
        RunEntry e; e.zoneIndex = (uint8_t)i; e.startEpoch = 1750000000u + i;
        e.centigallons = (uint16_t)(i * 100); e.result = RunResult::Completed;
        e.clockWasValid = true;
        a.push(e);
    }
    uint8_t blob[tinkle::RUNLOG_BLOB_BYTES];
    uint16_t len = a.serialize(blob, sizeof(blob));
    TEST_ASSERT_EQUAL_UINT16(3 * tinkle::RUNLOG_ENTRY_BYTES, len);

    RunLog b; b.deserialize(blob, len);
    TEST_ASSERT_EQUAL_UINT8(3, b.count());
    TEST_ASSERT_EQUAL_UINT8(2, b.head().zoneIndex);                 // newest
    TEST_ASSERT_EQUAL_UINT8(0, b.at(2).zoneIndex);                  // oldest
    TEST_ASSERT_EQUAL_UINT32(1750000002u, b.head().startEpoch);
    TEST_ASSERT_TRUE(b.head().clockWasValid);

    RunLog c;
    for (int i = 0; i < tinkle::RUNLOG_DEPTH + 4; ++i) {
        RunEntry e; e.zoneIndex = (uint8_t)i; c.push(e);
    }
    len = c.serialize(blob, sizeof(blob));
    TEST_ASSERT_EQUAL_UINT16(tinkle::RUNLOG_BLOB_BYTES, len);       // capped at a full ring
    RunLog d; d.deserialize(blob, len);
    TEST_ASSERT_EQUAL_UINT8(tinkle::RUNLOG_DEPTH, d.count());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(tinkle::RUNLOG_DEPTH + 3), d.head().zoneIndex);

    RunLog torn; torn.deserialize(blob, (uint16_t)(2 * tinkle::RUNLOG_ENTRY_BYTES + 4));
    TEST_ASSERT_EQUAL_UINT8(2, torn.count());                       // whole entries only

    RunLog empty; empty.deserialize(blob, 0);
    TEST_ASSERT_TRUE(empty.empty());
}

// NVS round-trip through Persistence: save the ring, reboot (fresh Persistence over the same
// store), rehydrate. One blob write to the additive `runlog` key, no schema_ver bump; an
// absent key reads back as an empty ring (read-with-default, DEC-008).
static void test_persist_runlog_roundtrip() {
    FakeKvStore s;
    Persistence p(s, 3);
    p.begin();

    RunLog absent;
    p.loadRunLog(absent);
    TEST_ASSERT_TRUE(absent.empty());                              // read-with-default

    RunLog log;
    for (int i = 0; i < 4; ++i) {
        RunEntry e; e.zoneIndex = (uint8_t)i; e.durationSec = (uint16_t)(60 + i);
        e.centigallons = (uint16_t)(i * 250); e.fertigate = (i % 2 == 0);
        e.result = (i == 3) ? RunResult::Faulted : RunResult::Completed;
        e.faultCode = (i == 3) ? (uint8_t)Fault::NoFlow : 0;
        e.startEpoch = 1750000000u + i; e.clockWasValid = true;
        log.push(e);
    }
    p.saveRunLog(log);
    TEST_ASSERT_EQUAL_INT(1, s.writesTo("runlog"));                // one packed blob write

    Persistence p2(s, 3);
    p2.begin();
    RunLog loaded;
    p2.loadRunLog(loaded);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.count());
    const RunEntry& h = loaded.head();                            // newest = i==3
    TEST_ASSERT_EQUAL_UINT8(3, h.zoneIndex);
    TEST_ASSERT_EQUAL(RunResult::Faulted, h.result);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Fault::NoFlow, h.faultCode);
    TEST_ASSERT_EQUAL_UINT16(750, h.centigallons);
    TEST_ASSERT_EQUAL_UINT32(1750000003u, h.startEpoch);
    TEST_ASSERT_TRUE(h.clockWasValid);
    const RunEntry o = loaded.at(3);                              // oldest = i==0
    TEST_ASSERT_EQUAL_UINT8(0, o.zoneIndex);
    TEST_ASSERT_TRUE(o.fertigate);
    TEST_ASSERT_EQUAL_UINT32(Persistence::SCHEMA_VER, s.getU32("schema_ver", 0));
}

// A full run pushes one entry carrying the pushed-in start epoch + volume + RunController's
// own zone/duration/fert/result, and sets the dirty flag for the debounced NVS write.
static void test_rc_runlog_records_run() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    TEST_ASSERT_TRUE(rc.runLog().empty());
    TEST_ASSERT_FALSE(rc.runLogDirty());

    const uint32_t now = runOnce(rc, 0, 1, 3, true, 1750000000u, true, 1234);
    (void)now;

    TEST_ASSERT_EQUAL_UINT8(1, rc.runLog().count());
    const RunEntry& e = rc.lastRun();
    TEST_ASSERT_EQUAL(RunResult::Completed, e.result);
    TEST_ASSERT_EQUAL_UINT8(1, e.zoneIndex);
    TEST_ASSERT_EQUAL_UINT16(3, e.durationSec);
    TEST_ASSERT_EQUAL_UINT16(1234, e.centigallons);
    TEST_ASSERT_TRUE(e.fertigate);
    TEST_ASSERT_TRUE(e.clockWasValid);
    TEST_ASSERT_EQUAL_UINT32(1750000000u, e.startEpoch);
    TEST_ASSERT_EQUAL_UINT8(0, e.faultCode);                      // Fault::None
    TEST_ASSERT_TRUE(rc.runLogDirty());
}

// clockWasValid is set only when the source was synced AND the epoch is plausible (>=2025):
// a pre-2025 free-run epoch and a synced-but-flagged-invalid start both store the epoch with
// the bit CLEAR, never as a bogus wall-clock (DEC-018 epoch-sanity guard).
static void test_rc_runlog_clockvalid_and_epoch_guard() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    uint32_t now = 0;
    now = runOnce(rc, now, 0, 1, false, 1700000000u, true, 0);    // 2023: pre-2025 -> clear
    now = runOnce(rc, now, 0, 1, false, 1750000000u, false, 0);   // 2025 but unsynced -> clear
    now = runOnce(rc, now, 0, 1, false, 1750000000u, true, 0);    // 2025 + synced -> set

    TEST_ASSERT_EQUAL_UINT8(3, rc.runLog().count());
    TEST_ASSERT_TRUE(rc.runLog().at(0).clockWasValid);            // newest (synced, plausible)
    TEST_ASSERT_EQUAL_UINT32(1750000000u, rc.runLog().at(0).startEpoch);
    TEST_ASSERT_FALSE(rc.runLog().at(1).clockWasValid);           // unsynced
    TEST_ASSERT_FALSE(rc.runLog().at(2).clockWasValid);           // pre-2025
    TEST_ASSERT_EQUAL_UINT32(1700000000u, rc.runLog().at(2).startEpoch);   // stored, just flagged
}

// A fault mid-run also pushes a ring entry (result Faulted + code), carrying the volume noted
// up to the fault — not a stale/zero value — so lastRun() stays the one source of truth.
static void test_rc_runlog_fault_pushes_entry() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    RunRequest req; req.zoneIndex = 1; req.durationSec = 30; req.fertigate = false;
    rc.requestRun(req, 0);
    uint32_t now = pump(rc, 0, 5, 8000, [&]{ return rc.state() == RunState::Running; });
    rc.noteRunStart(1750000000u, true);
    rc.noteRunVolume(500);                                        // 5.00 gal before the fault
    rc.raiseFault(Fault::NoFlow, now);

    TEST_ASSERT_EQUAL_UINT8(1, rc.runLog().count());
    const RunEntry& e = rc.lastRun();
    TEST_ASSERT_EQUAL(RunResult::Faulted, e.result);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Fault::NoFlow, e.faultCode);
    TEST_ASSERT_EQUAL_UINT16(500, e.centigallons);
    TEST_ASSERT_EQUAL_UINT8(1, e.zoneIndex);
    TEST_ASSERT_TRUE(rc.runLogDirty());
}

// lastRun() IS the ring head — the same object, and the most recent of several runs.
static void test_rc_lastrun_is_ring_head() {
    ValveDriver vd(g, cfg);
    RunController rc(vd, makeRunCfg());
    rc.begin(0);
    uint32_t now = 0;
    now = runOnce(rc, now, 0, 1, false, 1750000000u, true, 100);
    now = runOnce(rc, now, 1, 1, false, 1750000100u, true, 200);
    TEST_ASSERT_EQUAL_UINT8(2, rc.runLog().count());
    TEST_ASSERT_EQUAL_UINT8(1, rc.lastRun().zoneIndex);
    TEST_ASSERT_EQUAL_UINT16(200, rc.lastRun().centigallons);
    TEST_ASSERT_EQUAL_PTR(&rc.runLog().head(), &rc.lastRun());    // one source of truth
}

// --- GET /api/history (DEC-018, #70) ---------------------------------------------

// Empty rings, then a populated mix: run ring newest-first with per-entry clockWasValid,
// the fault ring as-is, and the render flags (clockValid + uptimeMs).
static void test_api_history_shape() {
    ApiRig r;
    JsonDocument out;

    // Empty: both rings empty, clock not yet synced — uptime still surfaced for the renderer.
    TEST_ASSERT_EQUAL_INT(200, r.api.getHistory(out, 4242));
    TEST_ASSERT_EQUAL_INT(0, out["runs"].size());
    TEST_ASSERT_EQUAL_INT(0, out["faults"].size());
    TEST_ASSERT_FALSE(out["clockValid"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(4242, out["uptimeMs"].as<uint32_t>());

    // Sync the clock (top-level flag), lay down two completed runs + one fault-log note.
    r.wallSrc.available = true;
    r.wallSrc.epochVal  = 1750000000u;
    r.clk.begin(0);
    uint32_t now = 0;
    now = runOnce(r.rc, now, 0, 1, false, 1750000000u, true, 250);    // 2.50 gal, synced
    now = runOnce(r.rc, now, 1, 2, true,  0,           false, 500);   // 5.00 gal, unsynced, fert
    r.fm.note(Fault::ValveRest, 1750000000u, true);

    out.clear();
    TEST_ASSERT_EQUAL_INT(200, r.api.getHistory(out, 9000));
    JsonArray runs = out["runs"];
    TEST_ASSERT_EQUAL_INT(2, runs.size());
    // Newest-first: the zone-1 fert run is the head.
    TEST_ASSERT_EQUAL_INT(1, runs[0]["zone"].as<int>());
    TEST_ASSERT_EQUAL_STRING("completed", runs[0]["result"].as<const char*>());
    TEST_ASSERT_TRUE(runs[0]["fertigate"].as<bool>());
    TEST_ASSERT_FALSE(runs[0]["clockWasValid"].as<bool>());
    TEST_ASSERT_EQUAL_INT(2, runs[0]["durationSec"].as<int>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, runs[0]["gallons"].as<float>());
    TEST_ASSERT_EQUAL_STRING("none", runs[0]["fault"].as<const char*>());
    // Older entry: the synced zone-0 run carries its wall-clock epoch.
    TEST_ASSERT_EQUAL_INT(0, runs[1]["zone"].as<int>());
    TEST_ASSERT_TRUE(runs[1]["clockWasValid"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(1750000000u, runs[1]["startEpoch"].as<uint32_t>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, runs[1]["gallons"].as<float>());

    JsonArray faults = out["faults"];
    TEST_ASSERT_EQUAL_INT(1, faults.size());
    TEST_ASSERT_EQUAL_STRING("valveRest", faults[0]["code"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(1750000000u, faults[0]["epoch"].as<uint32_t>());
    TEST_ASSERT_TRUE(faults[0]["clockWasValid"].as<bool>());

    TEST_ASSERT_TRUE(out["clockValid"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(9000, out["uptimeMs"].as<uint32_t>());
}

// A faulted run surfaces in history with result "faulted" + its fault code and the volume
// measured up to the fault.
static void test_api_history_faulted_run() {
    ApiRig r;
    RunRequest req; req.zoneIndex = 0; req.durationSec = 30; req.fertigate = false;
    r.rc.requestRun(req, 0);
    uint32_t now = pump(r.rc, 0, 5, 8000, [&]{ return r.rc.state() == RunState::Running; });
    r.rc.noteRunStart(1750000000u, true);
    r.rc.noteRunVolume(125);
    r.rc.raiseFault(Fault::NoFlow, now);

    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.getHistory(out, now));
    TEST_ASSERT_EQUAL_INT(1, out["runs"].size());
    TEST_ASSERT_EQUAL_STRING("faulted", out["runs"][0]["result"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("noFlow",  out["runs"][0]["fault"].as<const char*>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.25f, out["runs"][0]["gallons"].as<float>());
}

// Max depth stays sane: 32 runs serialize to exactly RUNLOG_DEPTH entries, newest-first.
static void test_api_history_full_ring() {
    ApiRig r;
    uint32_t now = 0;
    for (int i = 0; i < tinkle::RUNLOG_DEPTH + 3; ++i)
        now = runOnce(r.rc, now, (uint8_t)(i % 2), 1, false,
                      1750000000u + i, true, (uint16_t)(i * 10));
    JsonDocument out;
    TEST_ASSERT_EQUAL_INT(200, r.api.getHistory(out, now));
    TEST_ASSERT_EQUAL_INT(tinkle::RUNLOG_DEPTH, out["runs"].size());
    TEST_ASSERT_EQUAL_UINT32(1750000000u + (tinkle::RUNLOG_DEPTH + 2),
                             out["runs"][0]["startEpoch"].as<uint32_t>());   // newest is the head
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_safe_levels);
    RUN_TEST(test_open_zone_holds);
    RUN_TEST(test_close_zone_deenergizes);
    RUN_TEST(test_independent_timers);
    RUN_TEST(test_safe_state_during_open);
    RUN_TEST(test_diverter_legs);
    RUN_TEST(test_pump);
    RUN_TEST(test_safe_state);
    RUN_TEST(test_zone_bounds);
    RUN_TEST(test_millis_rollover);

    RUN_TEST(test_rc_begin_idle_safe);
    RUN_TEST(test_rc_full_sequence_completes);
    RUN_TEST(test_rc_diverter_skip_when_unchanged);
    RUN_TEST(test_rc_diverter_returns_plain_at_settle);
    RUN_TEST(test_rc_run_waits_residual_diverter_return);
    RUN_TEST(test_rc_plain_run_settle_no_diverter_travel);
    RUN_TEST(test_rc_first_plain_run_skips_diverter);
    RUN_TEST(test_rc_stop_unwinds);
    RUN_TEST(test_rc_fault_latches_and_clears);
    RUN_TEST(test_rc_fault_from_idle);
    RUN_TEST(test_rc_idle_fault_logs_no_phantom);
    RUN_TEST(test_rc_queue_runs_sequentially);
    RUN_TEST(test_rc_rejects_bad_requests);
    RUN_TEST(test_rc_sw_max_runtime_clamps);
    RUN_TEST(test_rc_watchdog_pre_open_gate_holds_then_proceeds);
    RUN_TEST(test_rc_watchdog_stuck_line_skips_run_keeps_queue);
    RUN_TEST(test_rc_watchdog_abort_preserves_queue_never_latches);
    RUN_TEST(test_rc_watchdog_verdict_wiring_holds_in_prep);
    RUN_TEST(test_rc_stop_wins_over_abort_log);
    RUN_TEST(test_rc_fault_during_prep);
    RUN_TEST(test_rc_stop_during_prep);
    RUN_TEST(test_rc_queue_full_rejected);

    RUN_TEST(test_loop_monitor_within_budget);
    RUN_TEST(test_loop_monitor_counts_overruns);
    RUN_TEST(test_loop_monitor_reset);

    RUN_TEST(test_persist_defaults_on_empty_nvs);
    RUN_TEST(test_persist_schema_stamped_once);
    RUN_TEST(test_persist_roundtrip_across_reboot);
    RUN_TEST(test_persist_write_on_change);
    RUN_TEST(test_persist_zone_bounds);

    RUN_TEST(test_clock_invalid_until_sync);
    RUN_TEST(test_clock_syncs_and_derives_fields);
    RUN_TEST(test_clock_freeruns_between_syncs);
    RUN_TEST(test_clock_resync_throttled_then_corrects);
    RUN_TEST(test_clock_locks_on_when_source_appears);
    RUN_TEST(test_clock_epoch_from_civil);
    RUN_TEST(test_clock_minute_rolled_edge);

    RUN_TEST(test_sched_fires_due_entry);
    RUN_TEST(test_sched_skips_wrong_minute_day_disabled);
    RUN_TEST(test_sched_invalid_clock_no_fire);
    RUN_TEST(test_sched_idempotent_within_minute);
    RUN_TEST(test_sched_fert_first_run_of_day);
    RUN_TEST(test_sched_fert_overrides);
    RUN_TEST(test_sched_dropped_on_full_queue);
    RUN_TEST(test_sched_eval_now_on_edit);
    RUN_TEST(test_sched_add_capacity);
    RUN_TEST(test_wifi_blip_under_grace_no_reconnect);
    RUN_TEST(test_wifi_sustained_drop_reconnects_periodically);
    RUN_TEST(test_wifi_reassociation_rearms);
    RUN_TEST(test_wifi_recovery_survives_millis_wrap);
    RUN_TEST(test_dist_plan_basic);
    RUN_TEST(test_dist_plan_floor_and_cap);
    RUN_TEST(test_dist_plan_oversubscribed_invalid);
    RUN_TEST(test_dist_emits_cycle_all_zones_fert_first);
    RUN_TEST(test_dist_idempotent_across_evalnow);
    RUN_TEST(test_dist_either_or_entry_dormant);
    RUN_TEST(test_dist_dropped_counts);
    RUN_TEST(test_dist_invalid_enabled_falls_back_to_schedule);
    RUN_TEST(test_dist_day_boundary_resets_mask);
    RUN_TEST(test_dist_config_pack_roundtrip);

    RUN_TEST(test_sched_rc_fert_routing);

    RUN_TEST(test_persist_pulses_per_gallon);
    RUN_TEST(test_flow_gallons_from_k);
    RUN_TEST(test_flow_k_guard);
    RUN_TEST(test_flow_rate_gpm);
    RUN_TEST(test_flow_rate_decays_when_flow_stops);
    RUN_TEST(test_flow_reset_accumulation);
    RUN_TEST(test_flow_single_sample_zero_rate);
    RUN_TEST(test_flow_subhz_ticks_update_gallons);
    RUN_TEST(test_flow_full_ring_steady_rate);

    RUN_TEST(test_ff_no_flow_after_grace);
    RUN_TEST(test_ff_no_flow_healthy_rate);
    RUN_TEST(test_ff_unexpected_idle_flow);
    RUN_TEST(test_ff_idle_subthreshold_no_fault);
    RUN_TEST(test_ff_transition_states_never_fault);
    RUN_TEST(test_ff_grace_resets_per_run);
    RUN_TEST(test_ff_begin_seeds_boot_window);
    RUN_TEST(test_ff_post_run_idle_rebaseline);
    RUN_TEST(test_ff_trailing_draindown_no_fault_then_still_armed);
    RUN_TEST(test_ff_never_decaying_flow_still_faults);
    RUN_TEST(test_ff_default_threshold_ignores_draindown_traps_welded_relay);
    RUN_TEST(test_ff_integration_no_flow_faults_rc);

    RUN_TEST(test_cal_start_bounded_plain_run);
    RUN_TEST(test_cal_start_refused_when_busy);
    RUN_TEST(test_cal_happy_path_k_to_nvs);
    RUN_TEST(test_cal_finish_mid_run_stops);
    RUN_TEST(test_cal_reject_bad_volume);
    RUN_TEST(test_cal_reject_k_out_of_range);
    RUN_TEST(test_cal_finish_not_calibrating);
    RUN_TEST(test_cal_run_fault_aborts);
    RUN_TEST(test_cal_cancel);
    RUN_TEST(test_cal_tally_immune_to_chained_run);

    RUN_TEST(test_wdt_powerup_disarmed_frozen_line_never_arms);
    RUN_TEST(test_wdt_arms_on_edge_and_holds_with_beat);
    RUN_TEST(test_wdt_clean_disarm_no_trip);
    RUN_TEST(test_wdt_hard_max_lockout_holds_under_beat);
    RUN_TEST(test_wdt_lockout_releases_after_quiet);
    RUN_TEST(test_wdt_ceiling_resets_per_arm);
    RUN_TEST(test_wdt_millis_rollover);

    RUN_TEST(test_watchdog_emits_only_in_pump_window);
    RUN_TEST(test_watchdog_toggle_cadence_and_park);
    RUN_TEST(test_watchdog_trip_verdicts);
    RUN_TEST(test_watchdog_trip_qualification_filters_glitches);
    RUN_TEST(test_watchdog_full_run_heartbeat_within_pump);

    RUN_TEST(test_fm_clear_blocked_until_condition_resolves);
    RUN_TEST(test_fm_not_faulted_and_unconditioned_codes);
    RUN_TEST(test_fm_log_edges_and_ring_wrap);
    RUN_TEST(test_fm_epoch_guard_and_persist);

    RUN_TEST(test_rest_clean_close_no_flag);
    RUN_TEST(test_rest_stuck_valve_flags_zone);
    RUN_TEST(test_rest_flag_self_heals_on_clean_close);
    RUN_TEST(test_rest_chained_run_aborts_check);
    RUN_TEST(test_rest_threshold_boundary);
    RUN_TEST(test_rest_draindown_then_quiet_no_flag);
    RUN_TEST(test_rest_never_quiet_flags_via_cap);
    RUN_TEST(test_rest_fault_aborts_window_and_valverest_cannot_latch);
    RUN_TEST(test_fm_note_is_nonlatching);

    RUN_TEST(test_api_status_shape);
    RUN_TEST(test_api_run_validation_and_start);
    RUN_TEST(test_api_fault_gate_and_exemptions);
    RUN_TEST(test_api_schedule_roundtrip_persist_atomic);
    RUN_TEST(test_api_settings_get_post);
    RUN_TEST(test_api_distributed_roundtrip);
    RUN_TEST(test_api_distributed_rejects_invalid);
    RUN_TEST(test_api_distributed_disable_and_fault_gate);
    RUN_TEST(test_api_flow_override_dec015);
    RUN_TEST(test_flow_detector_mute);
    RUN_TEST(test_api_ota_gate_and_run_inhibit);
    RUN_TEST(test_api_ota_allowed_while_faulted);
    RUN_TEST(test_api_cal_endpoints);
    RUN_TEST(test_api_stop_voids_calibration);
    RUN_TEST(test_sched_pack_unpack_roundtrip);
    RUN_TEST(test_persist_override_and_creds_roundtrip);

    RUN_TEST(test_runlog_pack_unpack_roundtrip);
    RUN_TEST(test_runlog_ring_wrap);
    RUN_TEST(test_runlog_serialize_roundtrip);
    RUN_TEST(test_persist_runlog_roundtrip);
    RUN_TEST(test_rc_runlog_records_run);
    RUN_TEST(test_rc_runlog_clockvalid_and_epoch_guard);
    RUN_TEST(test_rc_runlog_fault_pushes_entry);
    RUN_TEST(test_rc_lastrun_is_ring_head);

    RUN_TEST(test_api_history_shape);
    RUN_TEST(test_api_history_faulted_run);
    RUN_TEST(test_api_history_full_ring);
    return UNITY_END();
}

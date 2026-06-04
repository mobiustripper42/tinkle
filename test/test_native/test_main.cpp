#include <unity.h>
#include <map>
#include <vector>
#include <utility>
#include "valve_driver.h"

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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_safe_levels);
    RUN_TEST(test_pulse_open_then_coast);
    RUN_TEST(test_pulse_close_then_coast);
    RUN_TEST(test_never_both_high_on_reversal);
    RUN_TEST(test_independent_timers);
    RUN_TEST(test_diverter_direction);
    RUN_TEST(test_master_and_pump);
    RUN_TEST(test_safe_state);
    RUN_TEST(test_zone_bounds);
    RUN_TEST(test_millis_rollover);
    return UNITY_END();
}

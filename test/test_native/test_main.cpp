#include <unity.h>

// Native test harness placeholder. Real suites land as src/core modules are
// built: run state machine (with fake clock + fake GPIO), scheduler, fert
// policy, flow math, calibration K. Run with: pio test -e native

void setUp() {}
void tearDown() {}

static void test_scaffold_placeholder() {
    TEST_ASSERT_TRUE(true);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_scaffold_placeholder);
    return UNITY_END();
}

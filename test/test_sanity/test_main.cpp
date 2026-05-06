// Sanity check that the native test environment is wired up correctly.
// Real tests for pixmob_protocol and beat-detection logic land in Block 2.

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_arithmetic_sanity(void) {
    TEST_ASSERT_EQUAL_INT(2, 1 + 1);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_arithmetic_sanity);
    return UNITY_END();
}

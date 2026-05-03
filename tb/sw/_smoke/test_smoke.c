/* test_smoke.c — minimal Unity test to prove the SW host-test pipeline.
 *
 * Owned by T0 build-skeleton. Phase-2 SW agents add real per-block tests
 * alongside this one; the _smoke test stays as the canary that the build
 * system itself works.
 *
 * Contract (see tb/sw/Makefile.inc): Unity's UNITY_END() returns 0 on
 * all-pass, non-zero on any failure. The harness propagates the exit code.
 */

#include "unity.h"

void setUp(void)    { /* per-test fixture stub */ }
void tearDown(void) { /* per-test teardown stub */ }

static void test_smoke_truth(void)
{
    TEST_ASSERT_EQUAL_INT(1, 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_smoke_truth);
    return UNITY_END();
}

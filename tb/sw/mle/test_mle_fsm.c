/* tb/sw/mle/test_mle_fsm.c — S3 MLE FSM state-transition tests.
 *
 * Test gate per docs/MIGRATION_PLAN.md §S3:
 *   FSM state transitions IDLE -> ATTACH_PENDING -> REGISTERED on
 *   D-LOC-UPDATE-ACCEPT received. Group-Attach happy-path transitions.
 */

#include "tetra/mle.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_fsm_idle_to_registered(void)
{
    MleSession s = {0};
    s.state = MleState_Idle;

    TEST_ASSERT_EQUAL_INT(MleState_AttachPending,
                          mle_fsm_step(&s, MleEvt_DemandReceived));
    TEST_ASSERT_EQUAL_INT(MleState_Registered,
                          mle_fsm_step(&s, MleEvt_AcceptSent));
}

static void test_fsm_group_attach_cycle(void)
{
    MleSession s = {0};
    s.state = MleState_Registered;

    TEST_ASSERT_EQUAL_INT(MleState_GroupAttach,
                          mle_fsm_step(&s, MleEvt_GrpDemandReceived));
    TEST_ASSERT_EQUAL_INT(MleState_Registered,
                          mle_fsm_step(&s, MleEvt_GrpAckSent));
}

static void test_fsm_detach_path(void)
{
    MleSession s = {0};
    s.state = MleState_Registered;

    TEST_ASSERT_EQUAL_INT(MleState_Detaching,
                          mle_fsm_step(&s, MleEvt_DetachReceived));
    TEST_ASSERT_EQUAL_INT(MleState_Idle,
                          mle_fsm_step(&s, MleEvt_DetachComplete));
}

static void test_fsm_invalid_event_no_transition(void)
{
    MleSession s = {0};
    s.state = MleState_Idle;
    /* IDLE + GrpAckSent — invalid, stays IDLE. */
    TEST_ASSERT_EQUAL_INT(MleState_Idle,
                          mle_fsm_step(&s, MleEvt_GrpAckSent));
    TEST_ASSERT_EQUAL_INT(MleState_Idle,
                          mle_fsm_step(&s, MleEvt_AcceptSent));
}

static void test_fsm_re_attach_from_registered(void)
{
    MleSession s = {0};
    s.state = MleState_Registered;
    /* REGISTERED + DemandReceived -> AttachPending (re-registration). */
    TEST_ASSERT_EQUAL_INT(MleState_AttachPending,
                          mle_fsm_step(&s, MleEvt_DemandReceived));
}

static void test_fsm_attach_pending_detach(void)
{
    MleSession s = {0};
    s.state = MleState_AttachPending;
    /* MS detaches before we accept. */
    TEST_ASSERT_EQUAL_INT(MleState_Detaching,
                          mle_fsm_step(&s, MleEvt_DetachReceived));
    TEST_ASSERT_EQUAL_INT(MleState_Idle,
                          mle_fsm_step(&s, MleEvt_DetachComplete));
}

static void test_session_alloc_and_lookup(void)
{
    Mle mle = {0};
    /* Bypass mle_init since the bus isn't relevant for session bookkeeping. */
    mle.cfg.fallback_gssi = 0x2F4D61u;

    MleSession *s1 = mle_session_alloc(&mle, 0x282FF4u);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL_UINT32(0x00282FF4u, s1->issi);
    TEST_ASSERT_EQUAL_INT(MleState_Idle, s1->state);
    TEST_ASSERT_EQUAL_UINT8(4, s1->gila_class);
    TEST_ASSERT_EQUAL_UINT8(1, s1->gila_lifetime);

    /* Lookup returns the same slot. */
    MleSession *s1b = mle_session_lookup(&mle, 0x282FF4u);
    TEST_ASSERT_EQUAL_PTR(s1, s1b);

    /* Second alloc returns the existing slot (idempotent). */
    MleSession *s1c = mle_session_alloc(&mle, 0x282FF4u);
    TEST_ASSERT_EQUAL_PTR(s1, s1c);

    /* Different ISSI -> different slot. */
    MleSession *s2 = mle_session_alloc(&mle, 0x282F91u);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_NOT_EQUAL(s1, s2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fsm_idle_to_registered);
    RUN_TEST(test_fsm_group_attach_cycle);
    RUN_TEST(test_fsm_detach_path);
    RUN_TEST(test_fsm_invalid_event_no_transition);
    RUN_TEST(test_fsm_re_attach_from_registered);
    RUN_TEST(test_fsm_attach_pending_detach);
    RUN_TEST(test_session_alloc_and_lookup);
    return UNITY_END();
}

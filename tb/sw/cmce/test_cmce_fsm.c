/* tb/sw/cmce/test_cmce_fsm.c — CMCE per-call FSM tests.
 *
 * Owned by S4 (S4-sw-cmce). Test gate per docs/MIGRATION_PLAN.md §S4:
 *
 *     "state-machine: U-SETUP → D-CALL-PROCEEDING → D-CONNECT → voice →
 *      U-RELEASE → D-RELEASE happy-path. Simulated MS, no real PDU bytes."
 *
 * The FSM is driven directly via cmce_fsm_apply() — no LLC/MsgBus loop
 * required (the M3 group-call happy-path is a state-only test). For
 * end-to-end MsgBus coverage, see test_cmce_d_nwrk_broadcast.c
 * (test_send_d_nwrk_broadcast_posts_to_bus).
 *
 * Surprising assertions (these are the behaviours documented in
 * cmce_fsm.c):
 *   - Duplicate cmce_call_alloc() for an in-use call_identifier is
 *     rejected (returns NULL + bumps stats.fsm_drops).
 *   - U-SETUP into a non-SetupPending slot bumps fsm_drops but still
 *     transitions the slot's state.
 *   - Release in any non-Idle state goes Releasing; a SECOND Release
 *     completes the close (slot freed).
 *   - TxDemand from Connected goes TxGranted; from any other state it
 *     drops.
 */

#include "tetra/cmce.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"
#include "unity.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Bus storage scaffolding.
 * ------------------------------------------------------------------------- */
#define Q_CAP        4u
#define MAX_PAYLOAD  256u
#define TOTAL_SLOTS  (Q_CAP * (size_t) MsgPrio__Count)

static MsgBusEntry g_entries[TOTAL_SLOTS];
static uint8_t     g_payloads[TOTAL_SLOTS * MAX_PAYLOAD];
static MsgBus      g_bus;
static Cmce        g_cmce;

void setUp(void)
{
    memset(&g_bus,  0, sizeof(g_bus));
    memset(&g_cmce, 0, sizeof(g_cmce));
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_payloads, 0, sizeof(g_payloads));
    const MsgBusCfg cfg = {
        .queue_cap_per_prio    = Q_CAP,
        .max_payload_bytes     = MAX_PAYLOAD,
        .entry_storage         = g_entries,
        .entry_storage_bytes   = sizeof(g_entries),
        .payload_storage       = g_payloads,
        .payload_storage_bytes = sizeof(g_payloads),
    };
    TEST_ASSERT_EQUAL_INT(0, msgbus_init(&g_bus, &cfg));

    const CmceCfg ccfg = {
        .nwrk_bcast_period_multiframes  = 10,
        .cell_re_select_parameters_seed = 0x5655,
        .cell_load_ca                   = 0,
    };
    TEST_ASSERT_EQUAL_INT(0, cmce_init(&g_cmce, &g_bus, &ccfg));
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Test 1: slot allocation + lookup.
 * ------------------------------------------------------------------------- */
static void test_slot_alloc_and_lookup(void)
{
    const TetraAddress peer = { .ssi = 0x282FF4, .ssi_type = SsiType_Issi };
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, 0x1234, &peer, 1);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->in_use);
    TEST_ASSERT_EQUAL_UINT16(0x1234, s->call_identifier);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_SetupPending, (int) s->state);

    CmceCallSlot *s2 = cmce_call_lookup(&g_cmce, 0x1234);
    TEST_ASSERT_EQUAL_PTR(s, s2);

    /* Duplicate alloc is rejected. */
    CmceCallSlot *dup = cmce_call_alloc(&g_cmce, 0x1234, &peer, 1);
    TEST_ASSERT_NULL(dup);
    TEST_ASSERT_EQUAL_size_t(1, g_cmce.stats.fsm_drops);
}

static void test_slot_table_full(void)
{
    /* Fill every slot. */
    const TetraAddress peer = { .ssi = 0x100000, .ssi_type = SsiType_Issi };
    for (size_t i = 0; i < CMCE_MAX_CALLS; ++i) {
        CmceCallSlot *s = cmce_call_alloc(&g_cmce, (uint16_t) (0x100 + i), &peer, 1);
        TEST_ASSERT_NOT_NULL(s);
    }
    /* Next alloc must fail. */
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, 0x999, &peer, 1);
    TEST_ASSERT_NULL(s);
}

/* ---------------------------------------------------------------------------
 * Test 2: M3 happy path
 *   U-SETUP → D-CALL-PROCEEDING → D-CONNECT → U-TX-DEMAND →
 *   D-TX-GRANTED → (voice) → U-RELEASE → D-RELEASE
 * ------------------------------------------------------------------------- */
static void test_m3_group_call_happy_path(void)
{
    const TetraAddress ms = { .ssi = 0x282FF4, .ssi_type = SsiType_Issi };
    const uint16_t cid    = 0x0AAA;

    /* Step 0: MS sends U-SETUP — BS allocates slot. */
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, cid, &ms, 1);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_SetupPending, (int) s->state);

    /* Apply U-SETUP — already SetupPending, no transition. */
    CmcePdu pdu; memset(&pdu, 0, sizeof(pdu));
    pdu.pdu_type        = CmcePdu_Setup;
    pdu.call_identifier = cid;
    CmceCallState st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_SetupPending, (int) st);

    /* Step 1: BS sends D-CALL-PROCEEDING. */
    pdu.pdu_type = CmcePdu_CallProceeding;
    st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Proceeding, (int) st);

    /* Step 2: BS sends D-CONNECT. */
    pdu.pdu_type = CmcePdu_Connect;
    st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Connected, (int) st);
    TEST_ASSERT_EQUAL_size_t(1, g_cmce.stats.connect_count);

    /* Step 3: MS sends U-TX-DEMAND. */
    pdu.pdu_type = CmcePdu_TxDemand;
    st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_TxGranted, (int) st);
    TEST_ASSERT_EQUAL_size_t(1, g_cmce.stats.tx_grant_count);

    /* (voice phase — opaque to CMCE FSM, handled by TmdSap/UMAC.) */

    /* Step 4: MS sends U-RELEASE. */
    pdu.pdu_type = CmcePdu_Release;
    st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Releasing, (int) st);

    /* Step 5: BS sends D-RELEASE — second Release completes close. */
    pdu.pdu_type = CmcePdu_Release;
    st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Idle, (int) st);
    TEST_ASSERT_FALSE(s->in_use);
    TEST_ASSERT_EQUAL_size_t(2, g_cmce.stats.release_count);
}

/* ---------------------------------------------------------------------------
 * Test 3: TxDemand out-of-state drops.
 * ------------------------------------------------------------------------- */
static void test_tx_demand_out_of_state_drops(void)
{
    const TetraAddress ms = { .ssi = 0x123456, .ssi_type = SsiType_Issi };
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, 0x1, &ms, 1);
    TEST_ASSERT_NOT_NULL(s);
    /* TxDemand from SetupPending — not allowed. */
    CmcePdu pdu; memset(&pdu, 0, sizeof(pdu));
    pdu.pdu_type        = CmcePdu_TxDemand;
    pdu.call_identifier = 0x1;
    CmceCallState st = cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_SetupPending, (int) st);
    TEST_ASSERT_TRUE(g_cmce.stats.fsm_drops > 0u);
}

/* ---------------------------------------------------------------------------
 * Test 4: cmce_call_release frees the slot.
 * ------------------------------------------------------------------------- */
static void test_call_release_frees_slot(void)
{
    const TetraAddress ms = { .ssi = 0x111111, .ssi_type = SsiType_Issi };
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, 0x42, &ms, 1);
    TEST_ASSERT_NOT_NULL(s);
    cmce_call_release(&g_cmce, s);
    TEST_ASSERT_FALSE(s->in_use);
    /* After release, lookup must miss. */
    TEST_ASSERT_NULL(cmce_call_lookup(&g_cmce, 0x42));
}

/* ---------------------------------------------------------------------------
 * Test 5: cmce_init bad args.
 * ------------------------------------------------------------------------- */
static void test_cmce_init_bad_args(void)
{
    Cmce dummy;
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmce_init(NULL, &g_bus, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmce_init(&dummy, NULL, NULL));
}

/* ---------------------------------------------------------------------------
 * Test 6: Setup direction mismatch — Setup with valid CmcePdu_Setup PDU
 * type encodes for both directions, but check that the FSM correctly
 * transitions from idle.
 * ------------------------------------------------------------------------- */
static void test_setup_pending_to_connected_through_proceeding(void)
{
    const TetraAddress ms = { .ssi = 0xABCDEF, .ssi_type = SsiType_Issi };
    CmceCallSlot *s = cmce_call_alloc(&g_cmce, 0x55, &ms, 1);
    TEST_ASSERT_NOT_NULL(s);

    /* D-CALL-PROCEEDING from SetupPending → Proceeding. */
    CmcePdu pdu; memset(&pdu, 0, sizeof(pdu));
    pdu.pdu_type        = CmcePdu_CallProceeding;
    pdu.call_identifier = 0x55;
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Proceeding,
                          (int) cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink));

    /* D-CONNECT from Proceeding → Connected. */
    pdu.pdu_type = CmcePdu_Connect;
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Connected,
                          (int) cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink));

    /* CallProceeding from Connected → drop, no transition. */
    pdu.pdu_type = CmcePdu_CallProceeding;
    size_t drops_before = g_cmce.stats.fsm_drops;
    TEST_ASSERT_EQUAL_INT((int) CmceCall_Connected,
                          (int) cmce_fsm_apply(&g_cmce, s, &pdu, CmceDir_Downlink));
    TEST_ASSERT_EQUAL_size_t(drops_before + 1u, g_cmce.stats.fsm_drops);
}

/* ---------------------------------------------------------------------------
 * Test 7: Full M3 path through MsgBus — exercise cmce_handle_tle_msg.
 *
 * The handler decodes a CMCE PDU embedded in a TleSapMsg LLC body, drives
 * the FSM, and (for U-SETUP) emits D-CALL-PROCEEDING back via the bus.
 * We tap the (TmaSap, TleSap) tuple to count downstream messages.
 * ------------------------------------------------------------------------- */
static size_t   g_tap_n;
static uint8_t  g_tap_first_payload_first_byte;

static void tap(const SapMsg *m, void *ctx)
{
    (void) ctx;
    if (m->len > 0 && m->payload != NULL) {
        if (g_tap_n == 0u) {
            g_tap_first_payload_first_byte = m->payload[0];
        }
    }
    g_tap_n++;
}

static void test_handle_tle_msg_emits_d_call_proceeding(void)
{
    /* Re-tap and re-init to add the tap on top of the existing wiring. */
    g_tap_n = 0;
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TmaSap, SapId_TleSap,
                                              tap, NULL));

    /* Build U-SETUP encoded as the LLC body of a TleSapMsg. */
    CmcePdu setup; memset(&setup, 0, sizeof(setup));
    setup.pdu_type        = CmcePdu_Setup;
    setup.area_selection  = 0;
    setup.basic_service_information = cmce_bsi_make(0, 0, 1, 0);
    setup.call_priority   = 4;
    setup.called_party_type_identifier = CmcePty_Ssi;
    setup.called_party_address_ssi     = 0x002F4D63u;

    uint8_t   sbuf[CMCE_PDU_BODY_MAX_BYTES] = {0};
    BitBuffer enc = bb_init_autoexpand(sbuf, sizeof(sbuf) * 8u);
    int n = cmce_pdu_encode(&enc, &setup, CmceDir_Uplink);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    TleSapMsg in; memset(&in, 0, sizeof(in));
    in.endpoint              = 1;
    in.addr.ssi              = 0x282FF4;
    in.addr.ssi_type         = SsiType_Issi;
    in.pdu.pdu_type          = LlcPdu_BL_DATA;
    in.pdu.body_len_bits     = (uint16_t) n;
    memcpy(in.pdu.body, sbuf, ((size_t) n + 7u) / 8u);

    /* The U-SETUP arrives via cmce_handle_tle_msg — no call_id is set
     * on the wire (U-SETUP carries called_party not call_identifier),
     * so the slot is allocated under call_identifier=0 (which is what
     * the decoded CmcePdu has). The follow-up D-CALL-PROCEEDING uses
     * the same call_identifier. This is a known limitation of the M3
     * test harness — the BS would normally allocate a fresh call_id at
     * U-SETUP time; that allocation logic is a Phase-G/4 deliverable. */
    TEST_ASSERT_EQUAL_INT(0, cmce_handle_tle_msg(&g_cmce, &in));

    /* Drain the bus: cmce should have posted exactly one downstream
     * message (the D-CALL-PROCEEDING). */
    int drained = 0;
    while (msgbus_dispatch_one(&g_bus) == 1) { ++drained; }
    TEST_ASSERT_EQUAL_INT(1, drained);
    TEST_ASSERT_EQUAL_size_t(1, g_tap_n);

    /* Verify the slot transitioned to SetupPending after the U-SETUP. */
    CmceCallSlot *s = cmce_call_lookup(&g_cmce, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT((int) CmceCall_SetupPending, (int) s->state);
}

/* ---------------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slot_alloc_and_lookup);
    RUN_TEST(test_slot_table_full);
    RUN_TEST(test_m3_group_call_happy_path);
    RUN_TEST(test_tx_demand_out_of_state_drops);
    RUN_TEST(test_call_release_frees_slot);
    RUN_TEST(test_cmce_init_bad_args);
    RUN_TEST(test_setup_pending_to_connected_through_proceeding);
    RUN_TEST(test_handle_tle_msg_emits_d_call_proceeding);
    return UNITY_END();
}

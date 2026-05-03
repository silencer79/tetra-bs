/* tb/sw/core_msgbus/test_core_msgbus.c — S0 unit tests.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Test-gate per
 * docs/MIGRATION_PLAN.md §S0:
 *
 *   - post-N-messages-priority-order
 *   - register-dispatch-roundtrip with (dest, sap) match/non-match
 *   - queue-overflow → drop counter
 *   - BitBuffer round-trip 1..32 bit widths
 *   - bit-exact UL#0 first 32 bits = 0x01_41_7F_A7 round-trip vs
 *     reference_demand_reassembly_bitexact.md Z.122
 *
 * 100% public-function coverage in core: every API in msgbus.h is
 * exercised at least once across the cases below.
 */

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
#define MAX_PAYLOAD  16u
#define TOTAL_SLOTS  (Q_CAP * (size_t) MsgPrio__Count)

static MsgBusEntry g_entries[TOTAL_SLOTS];
static uint8_t     g_payloads[TOTAL_SLOTS * MAX_PAYLOAD];
static MsgBus      g_bus;

/* Capture buffer for handlers — every dispatch appends an entry so
 * tests can assert order + payload bytes. */
typedef struct {
    SapId    src;
    SapId    dest;
    SapId    sap;
    uint16_t len;
    uint8_t  payload[MAX_PAYLOAD];
    void    *ctx_seen;
} CaptureRec;

#define CAP_CAP 32u
static CaptureRec g_cap[CAP_CAP];
static size_t     g_cap_count;

static void capture_handler(const SapMsg *msg, void *ctx)
{
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_LESS_THAN(CAP_CAP, g_cap_count);

    CaptureRec *r = &g_cap[g_cap_count++];
    r->src      = SAPMSG_SRC(msg);
    r->dest     = SAPMSG_DEST(msg);
    r->sap      = SAPMSG_SAP(msg);
    r->len      = SAPMSG_LEN(msg);
    r->ctx_seen = ctx;
    if (msg->len > 0 && msg->payload != NULL) {
        memcpy(r->payload, msg->payload, msg->len);
    }
}

/* ---------------------------------------------------------------------------
 * setUp / tearDown — Unity calls per test.
 * ------------------------------------------------------------------------- */

void setUp(void)
{
    memset(&g_bus,    0, sizeof(g_bus));
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_payloads, 0, sizeof(g_payloads));
    memset(g_cap,     0, sizeof(g_cap));
    g_cap_count = 0;

    const MsgBusCfg cfg = {
        .queue_cap_per_prio    = Q_CAP,
        .max_payload_bytes     = MAX_PAYLOAD,
        .entry_storage         = g_entries,
        .entry_storage_bytes   = sizeof(g_entries),
        .payload_storage       = g_payloads,
        .payload_storage_bytes = sizeof(g_payloads),
    };
    TEST_ASSERT_EQUAL_INT(0, msgbus_init(&g_bus, &cfg));
}

void tearDown(void) { /* nothing — bus is reset in setUp */ }

/* ---------------------------------------------------------------------------
 * Test: msgbus_init rejects bad cfg.
 * ------------------------------------------------------------------------- */
static void test_msgbus_init_rejects_bad_cfg(void)
{
    MsgBus       b;
    MsgBusCfg    cfg = {0};
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_init(&b, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_init(&b, &cfg));   /* zero caps */

    /* undersized storage */
    cfg.queue_cap_per_prio    = 4;
    cfg.max_payload_bytes     = 8;
    cfg.entry_storage         = g_entries;
    cfg.entry_storage_bytes   = 16; /* << need */
    cfg.payload_storage       = g_payloads;
    cfg.payload_storage_bytes = sizeof(g_payloads);
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_init(&b, &cfg));
}

/* ---------------------------------------------------------------------------
 * Test: post N=8 across priorities, dispatch in High → Normal → Low order,
 * FIFO within priority.
 * ------------------------------------------------------------------------- */
static void test_dispatch_priority_order(void)
{
    /* Catch-all handler so every dispatch registers in g_cap. */
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                             capture_handler, NULL));
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TleSap, SapId_TleSap,
                                             capture_handler, NULL));

    /* Post 3 Low, 3 Normal, 2 High in interleaved order; expect High
     * first (FIFO), then Normal (FIFO), then Low (FIFO). */
    const struct { MsgPriority p; uint8_t tag; SapId routing; } posts[] = {
        { MsgPrio_Low,    1, SapId_TmaSap },
        { MsgPrio_Normal, 2, SapId_TleSap },
        { MsgPrio_High,   3, SapId_TmaSap },
        { MsgPrio_Low,    4, SapId_TmaSap },
        { MsgPrio_Normal, 5, SapId_TleSap },
        { MsgPrio_High,   6, SapId_TmaSap },
        { MsgPrio_Low,    7, SapId_TmaSap },
        { MsgPrio_Normal, 8, SapId_TleSap },
    };

    for (size_t i = 0; i < sizeof(posts) / sizeof(posts[0]); ++i) {
        SapMsg m;
        sapmsg_init(&m, SapId_TmaSap, posts[i].routing, posts[i].routing,
                    &posts[i].tag, 1);
        TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, posts[i].p, &m));
    }

    TEST_ASSERT_EQUAL_size_t(8, msgbus_pending(&g_bus));

    /* Drain. */
    int dispatched = 0;
    while (msgbus_dispatch_one(&g_bus) == 1) {
        dispatched += 1;
    }
    TEST_ASSERT_EQUAL_INT(8, dispatched);
    TEST_ASSERT_EQUAL_size_t(0, msgbus_pending(&g_bus));
    TEST_ASSERT_EQUAL_size_t(8, g_cap_count);

    /* Expected tags in dispatch order: High(3, 6), Normal(2, 5, 8),
     * Low(1, 4, 7). */
    const uint8_t expect[] = { 3, 6, 2, 5, 8, 1, 4, 7 };
    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT8(expect[i], g_cap[i].payload[0]);
    }

    /* Empty bus dispatches return 0. */
    TEST_ASSERT_EQUAL_INT(0, msgbus_dispatch_one(&g_bus));
}

/* ---------------------------------------------------------------------------
 * Test: handler is keyed by (dest, sap). Mismatched messages dispatch
 * but invoke no handler.
 * ------------------------------------------------------------------------- */
static void test_register_dispatch_match_only(void)
{
    int  marker = 42;
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                             capture_handler, &marker));

    /* Match. */
    const uint8_t payload_match[3] = { 0xDE, 0xAD, 0xBE };
    SapMsg m_match;
    sapmsg_init(&m_match, SapId_TleSap, SapId_TmaSap, SapId_TmaSap,
                payload_match, sizeof(payload_match));
    TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, MsgPrio_Normal, &m_match));

    /* Same dest, wrong sap. */
    SapMsg m_wrong_sap;
    sapmsg_init(&m_wrong_sap, SapId_TleSap, SapId_TmaSap, SapId_TmdSap,
                NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, MsgPrio_Normal, &m_wrong_sap));

    /* Right sap, wrong dest. */
    SapMsg m_wrong_dest;
    sapmsg_init(&m_wrong_dest, SapId_TleSap, SapId_TleSap, SapId_TmaSap,
                NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, MsgPrio_Normal, &m_wrong_dest));

    /* Three dispatches, only one capture. */
    TEST_ASSERT_EQUAL_INT(1, msgbus_dispatch_one(&g_bus));
    TEST_ASSERT_EQUAL_INT(1, msgbus_dispatch_one(&g_bus));
    TEST_ASSERT_EQUAL_INT(1, msgbus_dispatch_one(&g_bus));

    TEST_ASSERT_EQUAL_size_t(1, g_cap_count);
    TEST_ASSERT_EQUAL_INT(SapId_TleSap, g_cap[0].src);
    TEST_ASSERT_EQUAL_INT(SapId_TmaSap, g_cap[0].dest);
    TEST_ASSERT_EQUAL_INT(SapId_TmaSap, g_cap[0].sap);
    TEST_ASSERT_EQUAL_UINT16(3, g_cap[0].len);
    TEST_ASSERT_EQUAL_HEX8(0xDE, g_cap[0].payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, g_cap[0].payload[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, g_cap[0].payload[2]);
    TEST_ASSERT_EQUAL_PTR(&marker, g_cap[0].ctx_seen);
}

/* ---------------------------------------------------------------------------
 * Test: queue overflow drops + counter increments.
 * ------------------------------------------------------------------------- */
static void test_queue_overflow_drops(void)
{
    /* Fill Normal bucket (cap = 4) — 5th post is dropped. */
    for (uint8_t i = 0; i < Q_CAP; ++i) {
        SapMsg m;
        sapmsg_init(&m, SapId_TmaSap, SapId_TmaSap, SapId_TmaSap, &i, 1);
        TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, MsgPrio_Normal, &m));
    }
    TEST_ASSERT_EQUAL_size_t(0, msgbus_drops(&g_bus, MsgPrio_Normal));

    uint8_t extra = 0xFFu;
    SapMsg m_extra;
    sapmsg_init(&m_extra, SapId_TmaSap, SapId_TmaSap, SapId_TmaSap, &extra, 1);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, msgbus_post(&g_bus, MsgPrio_Normal, &m_extra));
    TEST_ASSERT_EQUAL_size_t(1, msgbus_drops(&g_bus, MsgPrio_Normal));

    /* Other buckets unaffected. */
    TEST_ASSERT_EQUAL_size_t(0, msgbus_drops(&g_bus, MsgPrio_High));
    TEST_ASSERT_EQUAL_size_t(0, msgbus_drops(&g_bus, MsgPrio_Low));

    /* After dispatching one entry, a fresh post fits. */
    TEST_ASSERT_EQUAL_INT(1, msgbus_dispatch_one(&g_bus));
    TEST_ASSERT_EQUAL_INT(0, msgbus_post(&g_bus, MsgPrio_Normal, &m_extra));
    TEST_ASSERT_EQUAL_size_t(1, msgbus_drops(&g_bus, MsgPrio_Normal));
}

/* ---------------------------------------------------------------------------
 * Test: msgbus_post / register input validation.
 * ------------------------------------------------------------------------- */
static void test_msgbus_input_validation(void)
{
    /* Bad register args. */
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_register(NULL, SapId_TmaSap, SapId_TmaSap,
                                                   capture_handler, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_register(&g_bus, SapId_None, SapId_TmaSap,
                                                   capture_handler, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_register(&g_bus, SapId_TmaSap, SapId__Max,
                                                   capture_handler, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                                   NULL, NULL));

    /* Bad post args. */
    SapMsg m;
    sapmsg_init(&m, SapId_TmaSap, SapId_TmaSap, SapId_TmaSap, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_post(NULL, MsgPrio_Normal, &m));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_post(&g_bus, MsgPrio_Normal, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_post(&g_bus, (MsgPriority) 99, &m));

    /* Bad dest/sap. */
    SapMsg bad;
    sapmsg_init(&bad, SapId_TmaSap, SapId_None, SapId_TmaSap, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_post(&g_bus, MsgPrio_Normal, &bad));

    /* Payload too big. */
    uint8_t big[MAX_PAYLOAD + 4] = {0};
    SapMsg too_big;
    sapmsg_init(&too_big, SapId_TmaSap, SapId_TmaSap, SapId_TmaSap, big, sizeof(big));
    TEST_ASSERT_EQUAL_INT(-E2BIG, msgbus_post(&g_bus, MsgPrio_Normal, &too_big));

    /* Dispatch on uninitialised bus. */
    MsgBus uninit = {0};
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_dispatch_one(&uninit));
    TEST_ASSERT_EQUAL_INT(-EINVAL, msgbus_dispatch_one(NULL));
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, msgbus_drops(&uninit, MsgPrio_Normal));
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, msgbus_drops(&g_bus,  (MsgPriority) 99));
    TEST_ASSERT_EQUAL_size_t(0,        msgbus_pending(NULL));
}

/* ---------------------------------------------------------------------------
 * Test: registration cap (MSGBUS_REG_CAP).
 * ------------------------------------------------------------------------- */
static void test_register_cap(void)
{
    for (size_t i = 0; i < MSGBUS_REG_CAP; ++i) {
        TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                                  capture_handler, NULL));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                                   capture_handler, NULL));
}

/* ---------------------------------------------------------------------------
 * Test: BitBuffer round-trip every width 1..32.
 *
 * We stash one value at each width into a 528-bit buffer
 * (sum 1+2+...+32 = 528), then read them back and compare. Values
 * use a width-derived pattern that exercises both alignment and the
 * "value exceeds num_bits" mask.
 * ------------------------------------------------------------------------- */
static void test_bitbuffer_roundtrip_all_widths(void)
{
    uint8_t   buf[(528 + 7) / 8];
    BitBuffer bb = bb_init(buf, 528);
    TEST_ASSERT_EQUAL_size_t(528, bb_len_bits(&bb));
    TEST_ASSERT_EQUAL_size_t(0,   bb_pos_bits(&bb));

    for (uint8_t n = 1; n <= 32; ++n) {
        const uint32_t v = (n == 32u) ? 0xCAFEBABEu
                                      : ((1u << n) - 1u) ^ (uint32_t) (n * 0x13u);
        const uint32_t v_masked = (n == 32u) ? v : (v & ((1u << n) - 1u));
        bb_put_bits(&bb, v_masked, n);
    }
    TEST_ASSERT_EQUAL_size_t(528, bb_pos_bits(&bb));
    TEST_ASSERT_EQUAL_size_t(0,   bb_remaining(&bb));

    bb_seek_bits(&bb, 0);
    for (uint8_t n = 1; n <= 32; ++n) {
        const uint32_t v_in       = (n == 32u) ? 0xCAFEBABEu
                                              : ((1u << n) - 1u) ^ (uint32_t) (n * 0x13u);
        const uint32_t v_expected = (n == 32u) ? v_in : (v_in & ((1u << n) - 1u));
        const uint32_t v_out      = bb_get_bits(&bb, n);
        char           msg[64];
        snprintf(msg, sizeof(msg), "round-trip n=%u", (unsigned) n);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(v_expected, v_out, msg);
    }
}

/* ---------------------------------------------------------------------------
 * Test: BitBuffer underrun returns 0.
 * ------------------------------------------------------------------------- */
static void test_bitbuffer_underrun(void)
{
    uint8_t   buf[2] = {0xAB, 0xCD};
    BitBuffer bb     = bb_init(buf, 16);

    TEST_ASSERT_EQUAL_HEX32(0xABu, bb_get_bits(&bb, 8));
    TEST_ASSERT_EQUAL_size_t(8, bb_pos_bits(&bb));

    /* Asking for more than remaining returns 0 and does not advance. */
    TEST_ASSERT_EQUAL_HEX32(0u, bb_get_bits(&bb, 16));
    TEST_ASSERT_EQUAL_size_t(8, bb_pos_bits(&bb));

    TEST_ASSERT_EQUAL_HEX32(0xCDu, bb_get_bits(&bb, 8));
}

/* ---------------------------------------------------------------------------
 * Test: BitBuffer autoexpand grows `end` up to cap_bits.
 * ------------------------------------------------------------------------- */
static void test_bitbuffer_autoexpand(void)
{
    uint8_t   buf[4] = {0};
    BitBuffer bb     = bb_init_autoexpand(buf, 32);
    TEST_ASSERT_EQUAL_size_t(0, bb_len_bits(&bb));

    bb_put_bits(&bb, 0xA, 4);
    TEST_ASSERT_EQUAL_size_t(4, bb_pos_bits(&bb));
    TEST_ASSERT_EQUAL_size_t(4, bb_len_bits(&bb));

    bb_put_bits(&bb, 0xBC, 8);
    TEST_ASSERT_EQUAL_size_t(12, bb_pos_bits(&bb));

    bb_seek_bits(&bb, 0);
    TEST_ASSERT_EQUAL_HEX32(0xAu, bb_get_bits(&bb, 4));
    TEST_ASSERT_EQUAL_HEX32(0xBCu, bb_get_bits(&bb, 8));

    /* set_autoexpand toggle covers the API. */
    bb_set_autoexpand(&bb, false);
    bb_set_autoexpand(NULL, false);  /* must tolerate NULL */
}

/* ---------------------------------------------------------------------------
 * Test: bit-exact UL#0 first 32 bits = 0x01_41_7F_A7 round-trip.
 *
 * Source: docs/references/reference_demand_reassembly_bitexact.md Z.122
 *   "UL#0 hex: 01 41 7F A7 01 12 66 34 20 C1 22 60"
 *
 * We decode the first 32 bits MSB-first (8/8/8/8 split) and re-encode
 * into a fresh buffer, expecting byte-identical output.
 * ------------------------------------------------------------------------- */
static void test_bitbuffer_gold_ul0_first32(void)
{
    static const uint8_t ul0_first4[4] = { 0x01, 0x41, 0x7F, 0xA7 };

    /* Single 32-bit read. */
    uint8_t   src[4];
    memcpy(src, ul0_first4, sizeof(src));
    BitBuffer bb_in = bb_init(src, 32);
    const uint32_t v32 = bb_get_bits(&bb_in, 32);
    TEST_ASSERT_EQUAL_HEX32(0x01417FA7u, v32);

    /* Re-encode 32 bits into a clean buffer; must equal source bytes. */
    uint8_t   sink[4] = {0};
    BitBuffer bb_out  = bb_init(sink, 32);
    bb_put_bits(&bb_out, v32, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ul0_first4, sink, 4);

    /* And again split MSB-first into 4×8-bit slices to lock the
     * MSB-first byte ordering — bb_get_bits with n=8 must hand back
     * 0x01, 0x41, 0x7F, 0xA7 in that order. */
    bb_seek_bits(&bb_in, 0);
    TEST_ASSERT_EQUAL_HEX32(0x01u, bb_get_bits(&bb_in, 8));
    TEST_ASSERT_EQUAL_HEX32(0x41u, bb_get_bits(&bb_in, 8));
    TEST_ASSERT_EQUAL_HEX32(0x7Fu, bb_get_bits(&bb_in, 8));
    TEST_ASSERT_EQUAL_HEX32(0xA7u, bb_get_bits(&bb_in, 8));
}

/* ---------------------------------------------------------------------------
 * Test: SapId helpers (sap_id_name / sap_id_is_valid /
 * sap_prio_default).
 * ------------------------------------------------------------------------- */
static void test_sap_id_helpers(void)
{
    TEST_ASSERT_EQUAL_STRING("Tma",  sap_id_name(SapId_TmaSap));
    TEST_ASSERT_EQUAL_STRING("Tnmm", sap_id_name(SapId_TnmmSap));
    TEST_ASSERT_EQUAL_STRING("None", sap_id_name(SapId_None));
    TEST_ASSERT_EQUAL_STRING("?",    sap_id_name((SapId) 999));

    TEST_ASSERT_TRUE(sap_id_is_valid(SapId_TmaSap));
    TEST_ASSERT_FALSE(sap_id_is_valid(SapId_None));
    TEST_ASSERT_FALSE(sap_id_is_valid(SapId__Max));
    TEST_ASSERT_FALSE(sap_id_is_valid((SapId) -1));

    TEST_ASSERT_EQUAL_INT(MsgPrio_High,   sap_prio_default(SapId_TmaSap));
    TEST_ASSERT_EQUAL_INT(MsgPrio_High,   sap_prio_default(SapId_TlmbSap));
    TEST_ASSERT_EQUAL_INT(MsgPrio_Low,    sap_prio_default(SapId_TnmmSap));
    TEST_ASSERT_EQUAL_INT(MsgPrio_Normal, sap_prio_default(SapId_TleSap));
}

/* ---------------------------------------------------------------------------
 * Test: SapMsg accessor macros (SAPMSG_PAYLOAD_BYTE bounds-check,
 * SAPMSG_MATCH).
 * ------------------------------------------------------------------------- */
static void test_sapmsg_accessors(void)
{
    const uint8_t bytes[3] = { 0x11, 0x22, 0x33 };
    SapMsg m;
    sapmsg_init(&m, SapId_TleSap, SapId_TmaSap, SapId_TmaSap, bytes, 3);

    TEST_ASSERT_EQUAL_HEX8(0x11, SAPMSG_PAYLOAD_BYTE(&m, 0));
    TEST_ASSERT_EQUAL_HEX8(0x33, SAPMSG_PAYLOAD_BYTE(&m, 2));
    /* OOB access returns 0. */
    TEST_ASSERT_EQUAL_HEX8(0x00, SAPMSG_PAYLOAD_BYTE(&m, 99));

    TEST_ASSERT_TRUE(SAPMSG_MATCH(&m, SapId_TmaSap, SapId_TmaSap));
    TEST_ASSERT_FALSE(SAPMSG_MATCH(&m, SapId_TleSap, SapId_TmaSap));

    /* sapmsg_init_zero produces NULL payload. */
    SapMsg z;
    sapmsg_init_zero(&z, SapId_TmaSap, SapId_TleSap, SapId_TleSap);
    TEST_ASSERT_EQUAL_UINT16(0, SAPMSG_LEN(&z));
    TEST_ASSERT_NULL(SAPMSG_PAYLOAD(&z));
    TEST_ASSERT_EQUAL_HEX8(0, SAPMSG_PAYLOAD_BYTE(&z, 0));

    /* sapmsg_init NULL must not crash. */
    sapmsg_init(NULL, SapId_None, SapId_None, SapId_None, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Test: TdmaTime default + TetraAddress mask + EndpointId width.
 * ------------------------------------------------------------------------- */
static void test_types_widths(void)
{
    /* Field bit-widths from gold_field_values.md TODO-A. */
    TdmaTime def = TDMA_TIME_DEFAULT;
    TEST_ASSERT_EQUAL_UINT8(1, def.t);
    TEST_ASSERT_EQUAL_UINT8(1, def.f);
    TEST_ASSERT_EQUAL_UINT8(1, def.m);
    TEST_ASSERT_EQUAL_UINT16(0, def.h);

    /* TetraAddress.ssi is 24-bit; the upper 8 of the uint32_t MUST be 0. */
    TetraAddress a = { .ssi = 0x282FF4u, .ssi_type = SsiType_Issi };
    TEST_ASSERT_EQUAL_HEX32(0x282FF4u, a.ssi & TETRA_SSI_MASK_24);
    TEST_ASSERT_EQUAL_HEX32(0u,        a.ssi & ~TETRA_SSI_MASK_24);

    /* EndpointId is uint32_t (32-bit per gold_field_values.md). */
    EndpointId ep = 0x12345678u;
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, ep);
    TEST_ASSERT_EQUAL_size_t(4u, sizeof(EndpointId));

    /* Sanity: enum sizes are within int range. */
    TEST_ASSERT_TRUE((int) SsiType_EventLabel == 7);
    TEST_ASSERT_TRUE((int) BurstType_SDB == 3);
    TEST_ASSERT_TRUE((int) TrainingSequence_SyncTrainSeq == 5);
    TEST_ASSERT_TRUE((int) PhysicalChannel_Unallocated == 2);
    TEST_ASSERT_TRUE((int) LogicalChannel_AACH == 0);
}

/* ---------------------------------------------------------------------------
 * Runner.
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_msgbus_init_rejects_bad_cfg);
    RUN_TEST(test_dispatch_priority_order);
    RUN_TEST(test_register_dispatch_match_only);
    RUN_TEST(test_queue_overflow_drops);
    RUN_TEST(test_msgbus_input_validation);
    RUN_TEST(test_register_cap);
    RUN_TEST(test_bitbuffer_roundtrip_all_widths);
    RUN_TEST(test_bitbuffer_underrun);
    RUN_TEST(test_bitbuffer_autoexpand);
    RUN_TEST(test_bitbuffer_gold_ul0_first32);
    RUN_TEST(test_sap_id_helpers);
    RUN_TEST(test_sapmsg_accessors);
    RUN_TEST(test_types_widths);

    return UNITY_END();
}

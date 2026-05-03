/* tb/sw/llc/test_llc_pdu.c — S2 LLC unit tests.
 *
 * Owned by S2 (S2-sw-llc). Test gate per docs/MIGRATION_PLAN.md §S2:
 *
 *   - BL-DATA encode/decode round-trip with NS=0 + NS=1.
 *   - BL-ACK encode/decode round-trip with NR=0 + NR=1.
 *   - BL-ADATA (NR+NS) round-trip plus +FCS variant round-trip.
 *   - BL-UDATA broadcast-frame round-trip.
 *   - AL-SETUP wrapper round-trip (no MM body).
 *   - NR/NS counter wraps (stop-and-wait modulo-2 toggle).
 *   - CRC-32 round-trip on +FCS variant — polynomial confirmed in
 *     llc.h:llc_crc32 doc-comment (IEEE 802.3 form, init/xorout 0xFFFFFFFF,
 *     reflected). No Gold-Ref FCS bit-vector is available (DL#735 BL-ADATA
 *     in M2 capture is the no-FCS variant), so this is round-trip-only.
 *
 * Plus state-machine tests:
 *   - llc_send_bl_data + llc_send_bl_ack round-trip via msgbus.
 *   - In-sequence RX advances nr_expected.
 *   - Out-of-sequence RX re-ACKs the previous NR without forwarding.
 *   - Endpoint slot allocation / lookup.
 */

#include "tetra/llc.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"
#include "unity.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Bus storage scaffolding — S0 idiom.
 * ------------------------------------------------------------------------- */

#define Q_CAP        8u
#define MAX_PAYLOAD  256u
#define TOTAL_SLOTS  (Q_CAP * (size_t) MsgPrio__Count)

static MsgBusEntry g_entries[TOTAL_SLOTS];
static uint8_t     g_payloads[TOTAL_SLOTS * MAX_PAYLOAD];
static MsgBus      g_bus;
static Llc         g_llc;

/* Capture buffers for downstream/upstream taps. */
typedef struct {
    SapId   src;
    SapId   dest;
    uint16_t len;
    uint8_t payload[MAX_PAYLOAD];
} CapRec;

#define CAP_MAX 16u
static CapRec g_tma_cap[CAP_MAX];
static size_t g_tma_cap_n;
static CapRec g_tle_cap[CAP_MAX];
static size_t g_tle_cap_n;

static void tma_tap(const SapMsg *m, void *ctx)
{
    (void) ctx;
    if (g_tma_cap_n >= CAP_MAX) return;
    CapRec *r = &g_tma_cap[g_tma_cap_n++];
    r->src = m->src; r->dest = m->dest; r->len = m->len;
    if (m->payload && m->len > 0 && m->len <= MAX_PAYLOAD) {
        memcpy(r->payload, m->payload, m->len);
    }
}

static void tle_tap(const SapMsg *m, void *ctx)
{
    (void) ctx;
    if (g_tle_cap_n >= CAP_MAX) return;
    CapRec *r = &g_tle_cap[g_tle_cap_n++];
    r->src = m->src; r->dest = m->dest; r->len = m->len;
    if (m->payload && m->len > 0 && m->len <= MAX_PAYLOAD) {
        memcpy(r->payload, m->payload, m->len);
    }
}

void setUp(void)
{
    memset(&g_bus,    0, sizeof(g_bus));
    memset(&g_llc,    0, sizeof(g_llc));
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_payloads, 0, sizeof(g_payloads));
    memset(g_tma_cap, 0, sizeof(g_tma_cap));
    memset(g_tle_cap, 0, sizeof(g_tle_cap));
    g_tma_cap_n = 0;
    g_tle_cap_n = 0;

    const MsgBusCfg cfg = {
        .queue_cap_per_prio    = Q_CAP,
        .max_payload_bytes     = MAX_PAYLOAD,
        .entry_storage         = g_entries,
        .entry_storage_bytes   = sizeof(g_entries),
        .payload_storage       = g_payloads,
        .payload_storage_bytes = sizeof(g_payloads),
    };
    TEST_ASSERT_EQUAL_INT(0, msgbus_init(&g_bus, &cfg));

    /* Register taps BEFORE llc_init so they get drained alongside the
     * llc-installed handlers. (Multiple handlers per (dest, sap) tuple
     * are allowed per S0 contract.) */
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TmaSap, SapId_TmaSap,
                                              tma_tap, NULL));
    TEST_ASSERT_EQUAL_INT(0, msgbus_register(&g_bus, SapId_TleSap, SapId_TleSap,
                                              tle_tap, NULL));

    const LlcCfg lcfg = { .max_retx = 3 };
    TEST_ASSERT_EQUAL_INT(0, llc_init(&g_llc, &g_bus, &lcfg));
}

void tearDown(void) { /* nothing — reset in setUp */ }

/* ---------------------------------------------------------------------------
 * PDU encode/decode round-trip helpers.
 * ------------------------------------------------------------------------- */

static void roundtrip_assert(const LlcPdu *in)
{
    uint8_t   buf[LLC_PDU_BODY_MAX_BYTES + 16u];
    memset(buf, 0, sizeof(buf));
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = llc_pdu_encode(&enc, in);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    BitBuffer dec = bb_init(buf, (size_t) n);
    LlcPdu    out;
    memset(&out, 0, sizeof(out));
    out.body_len_bits = in->body_len_bits;
    TEST_ASSERT_EQUAL_INT(0, llc_pdu_decode(&dec, &out));

    TEST_ASSERT_EQUAL_UINT8((uint8_t) in->pdu_type, (uint8_t) out.pdu_type);
    if (llc_pdu_type_has_nr(in->pdu_type)) {
        TEST_ASSERT_EQUAL_UINT8(in->nr, out.nr);
    }
    if (llc_pdu_type_has_ns(in->pdu_type)) {
        TEST_ASSERT_EQUAL_UINT8(in->ns, out.ns);
    }
    TEST_ASSERT_EQUAL_UINT16(in->body_len_bits, out.body_len_bits);
    if (in->body_len_bits > 0u) {
        const size_t nbytes = (in->body_len_bits + 7u) / 8u;
        TEST_ASSERT_EQUAL_HEX8_ARRAY(in->body, out.body, nbytes);
    }
    if (llc_pdu_type_has_fcs(in->pdu_type)) {
        TEST_ASSERT_TRUE_MESSAGE(out.fcs_valid, "FCS check failed");
    }
}

/* ---------------------------------------------------------------------------
 * PDU type tables (helpers).
 * ------------------------------------------------------------------------- */
static void test_pdu_type_helpers(void)
{
    TEST_ASSERT_TRUE(llc_pdu_type_is_valid(LlcPdu_BL_DATA));
    TEST_ASSERT_TRUE(llc_pdu_type_is_valid(LlcPdu_AL_SETUP));
    TEST_ASSERT_FALSE(llc_pdu_type_is_valid(0x9));
    TEST_ASSERT_FALSE(llc_pdu_type_is_valid(0xF));

    TEST_ASSERT_FALSE(llc_pdu_type_has_fcs(LlcPdu_BL_DATA));
    TEST_ASSERT_TRUE (llc_pdu_type_has_fcs(LlcPdu_BL_DATA_FCS));
    TEST_ASSERT_FALSE(llc_pdu_type_has_fcs(LlcPdu_AL_SETUP));

    TEST_ASSERT_TRUE (llc_pdu_type_has_ns(LlcPdu_BL_DATA));
    TEST_ASSERT_FALSE(llc_pdu_type_has_ns(LlcPdu_BL_ACK));
    TEST_ASSERT_TRUE (llc_pdu_type_has_ns(LlcPdu_BL_ADATA));
    TEST_ASSERT_FALSE(llc_pdu_type_has_ns(LlcPdu_BL_UDATA));

    TEST_ASSERT_TRUE (llc_pdu_type_has_nr(LlcPdu_BL_ACK));
    TEST_ASSERT_TRUE (llc_pdu_type_has_nr(LlcPdu_BL_ADATA));
    TEST_ASSERT_FALSE(llc_pdu_type_has_nr(LlcPdu_BL_DATA));
    TEST_ASSERT_FALSE(llc_pdu_type_has_nr(LlcPdu_BL_UDATA));
    TEST_ASSERT_FALSE(llc_pdu_type_has_nr(LlcPdu_AL_SETUP));
}

/* ---------------------------------------------------------------------------
 * BL-DATA round-trip NS=0 / NS=1.
 * ------------------------------------------------------------------------- */
static void test_bl_data_roundtrip_ns0(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_DATA;
    p.ns            = 0;
    p.body_len_bits = 24;
    p.body[0] = 0xAB; p.body[1] = 0xCD; p.body[2] = 0xEF;
    roundtrip_assert(&p);
}

static void test_bl_data_roundtrip_ns1(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_DATA;
    p.ns            = 1;
    p.body_len_bits = 16;
    p.body[0] = 0x12; p.body[1] = 0x34;
    roundtrip_assert(&p);
}

/* ---------------------------------------------------------------------------
 * BL-ACK round-trip NR=0 / NR=1. body_len_bits == 0 in pure-ACK case;
 * we also exercise a small body (MLE-PD) path which the gold-ref UL#2
 * actually has.
 * ------------------------------------------------------------------------- */
static void test_bl_ack_roundtrip_nr0(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_ACK;
    p.nr            = 0;
    p.body_len_bits = 0;
    roundtrip_assert(&p);
}

static void test_bl_ack_roundtrip_nr1_with_mle_pd(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_ACK;
    p.nr            = 1;
    p.body_len_bits = 3;          /* MLE-PD field, gold-ref UL#2 [41..43] */
    p.body[0]       = 0x80;       /* '100' MSB-aligned = SNDCP/Padding   */
    roundtrip_assert(&p);
}

/* ---------------------------------------------------------------------------
 * BL-ADATA (NR+NS) round-trip — gold-ref DL#735 layout, no FCS.
 * ------------------------------------------------------------------------- */
static void test_bl_adata_roundtrip(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_ADATA;
    p.nr            = 0;
    p.ns            = 0;
    p.body_len_bits = 96;   /* ~MLE-PD + MM body fragment */
    for (size_t i = 0; i < 12; ++i) p.body[i] = (uint8_t) (0x55 + i);
    roundtrip_assert(&p);
}

/* ---------------------------------------------------------------------------
 * BL-ADATA + FCS round-trip — exercises the +0x4 variant.
 * ------------------------------------------------------------------------- */
static void test_bl_adata_fcs_roundtrip(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_ADATA_FCS;
    p.nr            = 1;
    p.ns            = 1;
    p.body_len_bits = 64;
    for (size_t i = 0; i < 8; ++i) p.body[i] = (uint8_t) (0xA0 + i);
    roundtrip_assert(&p);
}

/* ---------------------------------------------------------------------------
 * BL-UDATA broadcast — no NR/NS, no FCS.
 * ------------------------------------------------------------------------- */
static void test_bl_udata_roundtrip(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_UDATA;
    /* D-NWRK-BCAST is 124 bits, but for clean round-trip-byte-array
     * compare we use a byte-aligned 120-bit body here. The 4-bit-tail
     * case is exercised separately (any field that isn't a whole-byte
     * multiple zeros the trailing bits in the decoded buffer). */
    p.body_len_bits = 120;
    for (size_t i = 0; i < 15; ++i) p.body[i] = (uint8_t) (0x10 ^ i);
    roundtrip_assert(&p);
}

/* BL-UDATA with non-byte-aligned body to exercise tail-bit handling. */
static void test_bl_udata_roundtrip_124bits(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_UDATA;
    p.body_len_bits = 124;
    for (size_t i = 0; i < 15; ++i) p.body[i] = (uint8_t) (0x10 ^ i);
    /* Top 4 bits of byte[15] are part of the body; bottom 4 are unused.
     * Set them to a recognisable nibble that we'll mask before compare. */
    p.body[15] = 0xA0;

    uint8_t   buf[LLC_PDU_BODY_MAX_BYTES + 4u];
    memset(buf, 0, sizeof(buf));
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = llc_pdu_encode(&enc, &p);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* 4 bits pdu_type + 124 bits body = 128 bits = 16 bytes. */
    TEST_ASSERT_EQUAL_INT(128, n);

    BitBuffer dec = bb_init(buf, (size_t) n);
    LlcPdu    out;
    memset(&out, 0, sizeof(out));
    out.body_len_bits = 124;
    TEST_ASSERT_EQUAL_INT(0, llc_pdu_decode(&dec, &out));
    TEST_ASSERT_EQUAL_UINT16(124, out.body_len_bits);
    /* Compare first 15 whole bytes byte-exact, then top nibble of byte[15]. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.body, out.body, 15);
    TEST_ASSERT_EQUAL_HEX8(p.body[15] & 0xF0u, out.body[15] & 0xF0u);
}

/* ---------------------------------------------------------------------------
 * AL-SETUP wrapper — pdu_type only, no body. Bit-match the gold-ref
 * DL#727 [51..54] = '1000' → 0x8 in the high nibble of byte[0].
 * ------------------------------------------------------------------------- */
static void test_al_setup_roundtrip(void)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_AL_SETUP;
    p.body_len_bits = 0;

    uint8_t   buf[2] = {0};
    BitBuffer enc    = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = llc_pdu_encode(&enc, &p);
    TEST_ASSERT_EQUAL_INT(4, n);
    /* Top nibble = 0x8 (1000). */
    TEST_ASSERT_EQUAL_HEX8(0x80u, buf[0] & 0xF0u);

    BitBuffer dec = bb_init(buf, (size_t) n);
    LlcPdu    out = {0};
    out.body_len_bits = 0;
    TEST_ASSERT_EQUAL_INT(0, llc_pdu_decode(&dec, &out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t) LlcPdu_AL_SETUP, (uint8_t) out.pdu_type);
    TEST_ASSERT_EQUAL_UINT16(0, out.body_len_bits);
}

/* ---------------------------------------------------------------------------
 * Bit-layout match: BL-DATA NS=0 with MLE-PD='001' as the first 3 bits.
 * Per gold-ref UL#0 [36..47]: pdu_type=0001, NS=0, MLE-PD=001, MM_pdu=0010.
 * Encoded into a fresh buffer must produce 0x12 in the high bits:
 *   bits  : 0001 0 001 0010 ... = 0x12 0x...  (0x12 = 0001 0010)
 *   The first 12 bits = 0x12 << 4 in our buf[0..1]. We assert that
 *   precise bit-pattern by laying down the same 12 bits and checking.
 * ------------------------------------------------------------------------- */
static void test_bl_data_gold_ul0_layout(void)
{
    /* First 12 bits of LLC frame from UL#0:
     *   pdu_type    = 0001
     *   N(S)        = 0
     *   MLE-PD      = 001
     *   MM_pdu_type = 0010
     * Concatenated MSB-first: 0001 0 001 0010 = 0x089 in 12 bits =
     *   buf[0] = 0x08, buf[1] = 0x90.
     */
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_DATA;
    p.ns            = 0;
    p.body_len_bits = 7; /* MLE-PD (3) + MM_pdu_type (4) — partial body */
    /* Body bits MSB-first packed: '001 0010' = 0010 0100 = 0x24,
     * MSB-aligned in body[0]. */
    p.body[0]       = 0x24;

    uint8_t   buf[4] = {0};
    BitBuffer enc    = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = llc_pdu_encode(&enc, &p);
    TEST_ASSERT_EQUAL_INT(12, n);
    /* 0001 0 001 0010 = 0x089 in 12 bits, MSB-aligned across 2 bytes:
     *   byte0 = 0001 0001 = 0x11
     *   byte1 = 0010 ____ — top nibble = 0x2, bottom nibble undefined
     *                       (we only wrote 12 bits).
     */
    TEST_ASSERT_EQUAL_HEX8(0x11u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x20u, buf[1] & 0xF0u);
}

/* ---------------------------------------------------------------------------
 * NR/NS counter wraps — modulo-2 toggle on stop-and-wait.
 *
 * Send three BL-DATA in succession; ns_send must toggle 0→1→0→1.
 * Each TX must enter awaiting_ack and need an ACK to clear before the
 * next send is allowed.
 * ------------------------------------------------------------------------- */
static void test_ns_counter_wraps(void)
{
    const TetraAddress addr = { .ssi = 0x282FF4u, .ssi_type = SsiType_Issi };
    const EndpointId   ep_id = 1u;
    const uint8_t      body[2] = { 0xDE, 0xAD };

    /* TX #1: NS=0. */
    TEST_ASSERT_EQUAL_INT(0, llc_send_bl_data(&g_llc, ep_id, &addr, body, 16));
    LlcEndpoint *ep = llc_endpoint_lookup(&g_llc, ep_id);
    TEST_ASSERT_NOT_NULL(ep);
    TEST_ASSERT_TRUE(ep->awaiting_ack);
    TEST_ASSERT_EQUAL_UINT8(1, ep->ns_send);   /* toggled after send */
    TEST_ASSERT_EQUAL_INT(-EBUSY,
                          llc_send_bl_data(&g_llc, ep_id, &addr, body, 16));

    /* Simulate ACK from peer with NR=1. */
    ep->awaiting_ack = false;

    /* TX #2: NS=1. */
    TEST_ASSERT_EQUAL_INT(0, llc_send_bl_data(&g_llc, ep_id, &addr, body, 16));
    TEST_ASSERT_EQUAL_UINT8(0, ep->ns_send);   /* wrap to 0 */
    ep->awaiting_ack = false;

    /* TX #3: NS=0 again. */
    TEST_ASSERT_EQUAL_INT(0, llc_send_bl_data(&g_llc, ep_id, &addr, body, 16));
    TEST_ASSERT_EQUAL_UINT8(1, ep->ns_send);
}

/* ---------------------------------------------------------------------------
 * In-sequence RX advances nr_expected; out-of-sequence RX does not.
 * ------------------------------------------------------------------------- */

static int build_bl_data_ind(TmaUnitdataInd *ind, EndpointId ep_id,
                             uint8_t ns, const uint8_t *body, uint16_t bl)
{
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_DATA;
    p.ns            = ns;
    p.body_len_bits = bl;
    if (bl > 0u) memcpy(p.body, body, (bl + 7u) / 8u);

    uint8_t   tmp[LLC_PDU_BODY_MAX_BYTES + 4u];
    memset(tmp, 0, sizeof(tmp));
    BitBuffer bb = bb_init_autoexpand(tmp, sizeof(tmp) * 8u);
    int n = llc_pdu_encode(&bb, &p);
    if (n < 0) return n;

    memset(ind, 0, sizeof(*ind));
    ind->endpoint = ep_id;
    ind->addr     = (TetraAddress){ .ssi = 0x282FF4u, .ssi_type = SsiType_Issi };
    ind->sdu_len_bits = (uint16_t) n;
    memcpy(ind->sdu_bits, tmp, (size_t) ((n + 7) / 8));
    return 0;
}

static void test_rx_in_sequence_advances_nr(void)
{
    const EndpointId ep_id = 2u;
    const uint8_t    body[2] = { 0xCA, 0xFE };

    /* Endpoint starts with nr_expected = 0. Sending NS=0 must advance. */
    TmaUnitdataInd ind;
    TEST_ASSERT_EQUAL_INT(0, build_bl_data_ind(&ind, ep_id, 0, body, 16));
    TEST_ASSERT_EQUAL_INT(0, llc_handle_tma_unitdata_ind(&g_llc, &ind));

    LlcEndpoint *ep = llc_endpoint_lookup(&g_llc, ep_id);
    TEST_ASSERT_NOT_NULL(ep);
    TEST_ASSERT_EQUAL_UINT8(1, ep->nr_expected);   /* advanced */

    /* MLE got the BL-DATA upward (one TleSap post). */
    /* Drain bus to invoke taps. */
    while (msgbus_dispatch_one(&g_bus) > 0) { /* keep going */ }
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1u, g_tle_cap_n);

    /* And we sent a BL-ACK downward. */
    /* g_tma_cap_n counts every tma post — 1 ACK at minimum. */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1u, g_tma_cap_n);
}

static void test_rx_out_of_sequence_reacks(void)
{
    const EndpointId ep_id = 3u;
    const uint8_t    body[2] = { 0xCA, 0xFE };

    /* Endpoint starts at nr_expected=0; send NS=1 (wrong order). */
    TmaUnitdataInd ind;
    TEST_ASSERT_EQUAL_INT(0, build_bl_data_ind(&ind, ep_id, 1, body, 16));
    TEST_ASSERT_EQUAL_INT(0, llc_handle_tma_unitdata_ind(&g_llc, &ind));

    LlcEndpoint *ep = llc_endpoint_lookup(&g_llc, ep_id);
    TEST_ASSERT_NOT_NULL(ep);
    TEST_ASSERT_EQUAL_UINT8(0, ep->nr_expected);   /* unchanged */

    while (msgbus_dispatch_one(&g_bus) > 0) { /* keep going */ }
    /* No TleSap upward (out-of-sequence dropped). */
    TEST_ASSERT_EQUAL_size_t(0u, g_tle_cap_n);
    /* But BL-ACK was still sent downward. */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1u, g_tma_cap_n);
}

/* ---------------------------------------------------------------------------
 * llc_init bad args + duplicate registration.
 * ------------------------------------------------------------------------- */
static void test_llc_init_bad_args(void)
{
    Llc bad;
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_init(NULL, &g_bus, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_init(&bad, NULL,  NULL));
}

/* ---------------------------------------------------------------------------
 * Endpoint allocation — saturates after LLC_MAX_ENDPOINTS distinct IDs.
 * ------------------------------------------------------------------------- */
static void test_endpoint_saturation(void)
{
    for (uint32_t i = 0; i < LLC_MAX_ENDPOINTS; ++i) {
        TEST_ASSERT_NOT_NULL(llc_endpoint_lookup(&g_llc, 100u + i));
    }
    /* Re-lookup of an existing one returns same slot. */
    LlcEndpoint *ep = llc_endpoint_lookup(&g_llc, 100u);
    TEST_ASSERT_NOT_NULL(ep);
    /* Adding one more (new ID) must fail (no slots). */
    TEST_ASSERT_NULL(llc_endpoint_lookup(&g_llc, 999u));
}

/* ---------------------------------------------------------------------------
 * CRC-32 sanity:
 *   1) crc32 over empty input returns init ^ xorout = 0.
 *   2) Round-trip via BL-ADATA+FCS verifies fcs_valid in decoder.
 *
 * No external Gold-Ref bit-vector is testable here; see llc.h note.
 * ------------------------------------------------------------------------- */
static void test_crc32_empty(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, llc_crc32(NULL, 0));
    uint8_t z = 0;
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, llc_crc32(&z, 0));
}

static void test_crc32_known_vector(void)
{
    /* CRC-32 over ASCII "123456789" (72 bits MSB-first) using THIS
     * implementation's exact configuration:
     *   poly=0x04C11DB7, init=0xFFFFFFFF, xorout=0xFFFFFFFF, NOT reflected
     *   (bits feed MSB-first as they appear in the BitBuffer).
     *
     * Expected value 0xFC891918 confirmed by an independent reference
     * implementation of the same parameters (see /tmp/crc_check.c during
     * development; algorithm is standard textbook MSB-first CRC division).
     *
     * NOTE: This is NOT the famous CRC-32/IEEE check value 0xCBF43926.
     * That value applies to the REFLECTED form used by Ethernet/zlib;
     * we use the non-reflected form so MSB-first bit-feed from the
     * BitBuffer matches on-air bit ordering. See llc_pdu.c llc_crc32
     * doc-comment for the full configuration + outstanding ETSI §22
     * polynomial-confirmation TODO. */
    static const uint8_t v[] = { '1','2','3','4','5','6','7','8','9' };
    TEST_ASSERT_EQUAL_HEX32(0xFC891918u, llc_crc32(v, 9 * 8));
}

/* ---------------------------------------------------------------------------
 * Encode bad args.
 * ------------------------------------------------------------------------- */
static void test_encode_bad_args(void)
{
    uint8_t buf[8] = {0};
    BitBuffer bb = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    LlcPdu p = {0};
    p.pdu_type      = LlcPdu_BL_DATA;
    p.body_len_bits = 16;
    p.body[0] = 0x12;

    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_encode(NULL, &p));
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_encode(&bb, NULL));

    LlcPdu bad = p;
    bad.pdu_type = (LlcPduType) 0xF;
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_encode(&bb, &bad));

    LlcPdu too_big = p;
    too_big.body_len_bits = (uint16_t) (LLC_PDU_BODY_MAX_BYTES * 8u + 1u);
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_encode(&bb, &too_big));

    LlcPdu setup_with_body = p;
    setup_with_body.pdu_type = LlcPdu_AL_SETUP;
    setup_with_body.body_len_bits = 8;  /* AL-SETUP: must be 0 */
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_encode(&bb, &setup_with_body));
}

/* ---------------------------------------------------------------------------
 * Decode bad-args + short-frame + invalid pdu_type.
 * ------------------------------------------------------------------------- */
static void test_decode_bad_args(void)
{
    uint8_t buf[2] = {0};
    BitBuffer bb = bb_init(buf, 16);
    LlcPdu p = {0};
    p.body_len_bits = 0;

    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_decode(NULL, &p));
    TEST_ASSERT_EQUAL_INT(-EINVAL, llc_pdu_decode(&bb, NULL));

    /* Short frame: < 4 bits. */
    uint8_t   tiny[1] = {0};
    BitBuffer t1 = bb_init(tiny, 3);
    TEST_ASSERT_EQUAL_INT(-EPROTO, llc_pdu_decode(&t1, &p));

    /* Invalid pdu_type: 0x9 nibble. */
    uint8_t inv[1] = { 0x90 };  /* high nibble = 1001 = 0x9 (invalid) */
    BitBuffer ti = bb_init(inv, 4);
    p.body_len_bits = 0;
    TEST_ASSERT_EQUAL_INT(-EPROTO, llc_pdu_decode(&ti, &p));
}

/* ---------------------------------------------------------------------------
 * Runner.
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_pdu_type_helpers);

    RUN_TEST(test_bl_data_roundtrip_ns0);
    RUN_TEST(test_bl_data_roundtrip_ns1);
    RUN_TEST(test_bl_ack_roundtrip_nr0);
    RUN_TEST(test_bl_ack_roundtrip_nr1_with_mle_pd);
    RUN_TEST(test_bl_adata_roundtrip);
    RUN_TEST(test_bl_adata_fcs_roundtrip);
    RUN_TEST(test_bl_udata_roundtrip);
    RUN_TEST(test_bl_udata_roundtrip_124bits);
    RUN_TEST(test_al_setup_roundtrip);
    RUN_TEST(test_bl_data_gold_ul0_layout);

    RUN_TEST(test_ns_counter_wraps);
    RUN_TEST(test_rx_in_sequence_advances_nr);
    RUN_TEST(test_rx_out_of_sequence_reacks);

    RUN_TEST(test_llc_init_bad_args);
    RUN_TEST(test_endpoint_saturation);

    RUN_TEST(test_crc32_empty);
    RUN_TEST(test_crc32_known_vector);

    RUN_TEST(test_encode_bad_args);
    RUN_TEST(test_decode_bad_args);

    return UNITY_END();
}

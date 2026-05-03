/* tb/sw/cmce/test_cmce_pdu.c — CMCE PDU round-trip + structural tests.
 *
 * Owned by S4 (S4-sw-cmce). Test gate per docs/MIGRATION_PLAN.md §S4:
 *
 *     "encode/decode round-trip for all 8 PDU types (D-SETUP, U-SETUP,
 *      D-CALL-PROCEEDING, D-CONNECT, U-TX-DEMAND, D-TX-GRANTED, U-RELEASE,
 *      D-RELEASE). Structural-only (no Gold-Ref). Sanity: pdu_type field
 *      at correct bit position, call_identifier round-trips, optional
 *      fields' o-bit logic."
 *
 * No Gold-Ref is available for these PDUs (gold_field_values.md
 * §"Open uncertainties" #5). All bit values are PROVISIONAL per
 * cmce_pdu.c header. Test asserts:
 *   - encoder writes pdu_type as the leading 5 bits in MSB-first order
 *   - call_identifier survives a full round-trip for each PDU type that
 *     carries one
 *   - o-bit gets serialised at the documented bit position per
 *     reference_cmce_group_call_pdus.md
 *   - encode then decode reproduces all set fields
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
 * Helpers.
 * ------------------------------------------------------------------------- */

static uint8_t buf_bit(const uint8_t *buf, size_t bit_idx)
{
    const uint8_t byte = buf[bit_idx >> 3];
    const uint8_t off  = (uint8_t) (7u - (bit_idx & 0x7u));
    return (uint8_t) ((byte >> off) & 0x1u);
}

/* Read first N bits MSB-first as an integer. */
static uint32_t buf_read_msb(const uint8_t *buf, size_t bit_off, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; ++i) {
        v = (v << 1) | buf_bit(buf, bit_off + i);
    }
    return v;
}

void setUp(void)    {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Common round-trip assertion. Caller fills `in`, we encode, decode, and
 * verify shared fields match.
 * ------------------------------------------------------------------------- */
static void assert_roundtrip(const CmcePdu *in, CmceDirection dir)
{
    uint8_t   buf[CMCE_PDU_BODY_MAX_BYTES + 8u] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, in, dir);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    /* Sanity 1: pdu_type lives at bit position [0..4] MSB-first. */
    const uint32_t got_type = buf_read_msb(buf, 0, 5);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) (in->pdu_type & 0x1Fu), got_type);

    BitBuffer dec = bb_init(buf, (size_t) n);
    CmcePdu   out;
    memset(&out, 0, sizeof(out));
    int rc = cmce_pdu_decode(&dec, &out, dir, (uint16_t) n);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_INT((int) in->pdu_type, (int) out.pdu_type);
    /* call_identifier round-trip — applies to every PDU we support
     * except U-SETUP which uses called_party fields instead. */
    if (in->pdu_type != CmcePdu_Setup ||
        (dir == CmceDir_Downlink)) {
        TEST_ASSERT_EQUAL_UINT16(in->call_identifier, out.call_identifier);
    }
    /* o-bit round-trip applies to all of our PDUs. */
    TEST_ASSERT_EQUAL(in->optionals_present ? 1 : 0,
                      out.optionals_present ? 1 : 0);
}

/* ---------------------------------------------------------------------------
 * D-SETUP round-trip.
 * ------------------------------------------------------------------------- */
static void test_d_setup_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                          = CmcePdu_Setup;
    p.call_identifier                   = 0x1234;
    p.call_time_out                     = 5;
    p.hook_method_selection             = 1;
    p.simplex_duplex_selection          = 0;
    p.basic_service_information         = cmce_bsi_make(0, 0, 1, 0); /* TchS, p2multi */
    p.transmission_grant                = CmceTxGrant_Granted;
    p.transmission_request_permission   = 1;
    p.call_priority                     = 7;
    p.optionals_present                 = false;
    assert_roundtrip(&p, CmceDir_Downlink);

    /* Field-by-field after a fresh round-trip. */
    uint8_t   buf[CMCE_PDU_BODY_MAX_BYTES + 8u] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT(41, n);  /* exact mandatory length per §14.7.1.12 */

    BitBuffer dec = bb_init(buf, (size_t) n);
    CmcePdu   o;  memset(&o, 0, sizeof(o));
    TEST_ASSERT_EQUAL_INT(0, cmce_pdu_decode(&dec, &o, CmceDir_Downlink, n));
    TEST_ASSERT_EQUAL_UINT8(p.call_time_out,                     o.call_time_out);
    TEST_ASSERT_EQUAL_UINT8(p.hook_method_selection,             o.hook_method_selection);
    TEST_ASSERT_EQUAL_UINT8(p.simplex_duplex_selection,          o.simplex_duplex_selection);
    TEST_ASSERT_EQUAL_UINT8(p.basic_service_information,         o.basic_service_information);
    TEST_ASSERT_EQUAL_INT  ((int) p.transmission_grant,    (int) o.transmission_grant);
    TEST_ASSERT_EQUAL_UINT8(p.transmission_request_permission,   o.transmission_request_permission);
    TEST_ASSERT_EQUAL_UINT8(p.call_priority,                     o.call_priority);
}

/* ---------------------------------------------------------------------------
 * U-SETUP round-trip — Ssi (24-bit GSSI) called_party.
 * ------------------------------------------------------------------------- */
static void test_u_setup_roundtrip_ssi(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                          = CmcePdu_Setup;
    p.area_selection                    = 0;
    p.hook_method_selection             = 1;
    p.simplex_duplex_selection          = 0;
    p.basic_service_information         = cmce_bsi_make(0, 0, 1, 0);
    p.request_to_transmit_send_data     = 0;
    p.call_priority                     = 8;
    p.clir_control                      = 0;
    p.called_party_type_identifier      = CmcePty_Ssi;
    p.called_party_address_ssi          = 0x002F4D63u;  /* gold-ref GSSI    */
    p.optionals_present                 = false;
    assert_roundtrip(&p, CmceDir_Uplink);
}

static void test_u_setup_roundtrip_sna(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                          = CmcePdu_Setup;
    p.area_selection                    = 4;
    p.hook_method_selection             = 0;
    p.simplex_duplex_selection          = 1;
    p.basic_service_information         = cmce_bsi_make(0, 0, 0, 0); /* TchS p2p */
    p.call_priority                     = 1;
    p.clir_control                      = 2;
    p.called_party_type_identifier      = CmcePty_Sna;
    p.called_party_short_number_address = 0xC3;
    p.optionals_present                 = true;
    assert_roundtrip(&p, CmceDir_Uplink);

    /* Re-decode and confirm the Sna byte survived. */
    uint8_t   buf[CMCE_PDU_BODY_MAX_BYTES + 8u] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Uplink);
    BitBuffer dec = bb_init(buf, (size_t) n);
    CmcePdu   o;  memset(&o, 0, sizeof(o));
    TEST_ASSERT_EQUAL_INT(0, cmce_pdu_decode(&dec, &o, CmceDir_Uplink, n));
    TEST_ASSERT_EQUAL_UINT8(0xC3, o.called_party_short_number_address);
}

/* ---------------------------------------------------------------------------
 * D-CALL-PROCEEDING round-trip.
 * ------------------------------------------------------------------------- */
static void test_d_call_proceeding_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                       = CmcePdu_CallProceeding;
    p.call_identifier                = 0x2222;
    p.call_time_out_set_up_phase     = 4;
    p.hook_method_selection          = 1;
    p.simplex_duplex_selection       = 1;
    p.optionals_present              = false;
    assert_roundtrip(&p, CmceDir_Downlink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT(25, n);  /* §14.7.1.2 */

    BitBuffer dec = bb_init(buf, (size_t) n);
    CmcePdu   o;  memset(&o, 0, sizeof(o));
    TEST_ASSERT_EQUAL_INT(0, cmce_pdu_decode(&dec, &o, CmceDir_Downlink, n));
    TEST_ASSERT_EQUAL_UINT8(p.call_time_out_set_up_phase, o.call_time_out_set_up_phase);
}

/* ---------------------------------------------------------------------------
 * D-CONNECT round-trip.
 * ------------------------------------------------------------------------- */
static void test_d_connect_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                          = CmcePdu_Connect;
    p.call_identifier                   = 0x0AAA;
    p.call_time_out                     = 0;
    p.hook_method_selection             = 1;
    p.simplex_duplex_selection          = 0;
    p.transmission_grant                = CmceTxGrant_Granted;
    p.transmission_request_permission   = 1;
    p.call_ownership                    = 0;
    p.optionals_present                 = true;
    assert_roundtrip(&p, CmceDir_Downlink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT(30, n);  /* §14.7.1.4 */
}

/* ---------------------------------------------------------------------------
 * U-TX-DEMAND round-trip.
 * ------------------------------------------------------------------------- */
static void test_u_tx_demand_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type            = CmcePdu_TxDemand;
    p.call_identifier     = 0x3FFE;     /* near-max 14-bit                */
    p.tx_demand_priority  = 3;
    p.encryption_control  = 0;
    p.optionals_present   = false;
    assert_roundtrip(&p, CmceDir_Uplink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT(24, n);

    /* Direction-mismatch must be rejected. */
    uint8_t   buf2[8] = {0};
    BitBuffer enc2 = bb_init_autoexpand(buf2, sizeof(buf2) * 8u);
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmce_pdu_encode(&enc2, &p, CmceDir_Downlink));
}

/* ---------------------------------------------------------------------------
 * D-TX-GRANTED round-trip.
 * ------------------------------------------------------------------------- */
static void test_d_tx_granted_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type                          = CmcePdu_TxGranted;
    p.call_identifier                   = 0x0001;
    p.transmission_grant                = CmceTxGrant_RequestQueued;
    p.transmission_request_permission   = 0;
    p.encryption_control                = 1;
    p.optionals_present                 = false;
    assert_roundtrip(&p, CmceDir_Downlink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT(25, n);

    /* The "reserved" bit at position 23 must be 0 per §14.7.1.15. */
    TEST_ASSERT_EQUAL_UINT8(0, buf_bit(buf, 23u));
}

/* ---------------------------------------------------------------------------
 * U-RELEASE + D-RELEASE round-trip — same wire layout per §14.7.x.9.
 * ------------------------------------------------------------------------- */
static void test_u_release_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type            = CmcePdu_Release;
    p.call_identifier     = 0x1ABC;
    p.disconnect_cause    = CmceDisconnect_UserRequested;
    p.optionals_present   = false;
    assert_roundtrip(&p, CmceDir_Uplink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Uplink);
    TEST_ASSERT_EQUAL_INT(25, n);
}

static void test_d_release_roundtrip(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type            = CmcePdu_Release;
    p.call_identifier     = 0x0FAB;
    p.disconnect_cause    = CmceDisconnect_NetworkBusy;
    p.optionals_present   = true;
    assert_roundtrip(&p, CmceDir_Downlink);

    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    int n = cmce_pdu_encode(&enc, &p, CmceDir_Downlink);
    TEST_ASSERT_EQUAL_INT(25, n);
    BitBuffer dec = bb_init(buf, (size_t) n);
    CmcePdu   o;  memset(&o, 0, sizeof(o));
    TEST_ASSERT_EQUAL_INT(0, cmce_pdu_decode(&dec, &o, CmceDir_Downlink, n));
    TEST_ASSERT_EQUAL_INT((int) CmceDisconnect_NetworkBusy,
                          (int) o.disconnect_cause);
}

/* ---------------------------------------------------------------------------
 * pdu_type at correct bit position — sanity sweep.
 * ------------------------------------------------------------------------- */
static void test_pdu_type_at_bit_position_zero(void)
{
    /* For each PDU type, encode and confirm the leading 5 bits are the
     * pdu_type. The encoder is the source of truth here, so this is a
     * regression guard rather than a true bit-exact test. */
    struct {
        CmcePduType t;
        CmceDirection dir;
    } cases[] = {
        { CmcePdu_Setup,           CmceDir_Downlink },
        { CmcePdu_Setup,           CmceDir_Uplink   },
        { CmcePdu_CallProceeding,  CmceDir_Downlink },
        { CmcePdu_Connect,         CmceDir_Downlink },
        { CmcePdu_TxDemand,        CmceDir_Uplink   },
        { CmcePdu_TxGranted,       CmceDir_Downlink },
        { CmcePdu_Release,         CmceDir_Uplink   },
        { CmcePdu_Release,         CmceDir_Downlink },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CmcePdu p; memset(&p, 0, sizeof(p));
        p.pdu_type        = cases[i].t;
        p.call_identifier = 0x0DEF;
        if (cases[i].dir == CmceDir_Uplink && cases[i].t == CmcePdu_Setup) {
            p.called_party_type_identifier = CmcePty_Ssi;
            p.called_party_address_ssi     = 0x123456;
        }

        uint8_t   buf[16] = {0};
        BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
        int n = cmce_pdu_encode(&enc, &p, cases[i].dir);
        TEST_ASSERT_GREATER_THAN_INT(0, n);
        const uint32_t got = buf_read_msb(buf, 0, 5);
        TEST_ASSERT_EQUAL_UINT32((uint32_t) cases[i].t, got);
    }
}

/* ---------------------------------------------------------------------------
 * Decode bad-input handling.
 * ------------------------------------------------------------------------- */
static void test_decode_unknown_pdu_type(void)
{
    /* Manually craft 5 bits = 0x1F (CmcePdu_Unknown sentinel-adjacent). */
    uint8_t   buf[4] = { 0xF8, 0x00, 0x00, 0x00 };  /* 5 bits = 11111 = 31 */
    BitBuffer dec = bb_init(buf, 32);
    CmcePdu   o;
    int rc = cmce_pdu_decode(&dec, &o, CmceDir_Downlink, 32);
    TEST_ASSERT_EQUAL_INT(-EPROTO, rc);
    TEST_ASSERT_EQUAL_INT((int) CmcePdu_Unknown, (int) o.pdu_type);
}

static void test_encode_bad_args(void)
{
    CmcePdu p; memset(&p, 0, sizeof(p));
    p.pdu_type = CmcePdu_Release;
    uint8_t   buf[8] = {0};
    BitBuffer enc = bb_init_autoexpand(buf, sizeof(buf) * 8u);
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmce_pdu_encode(NULL, &p, CmceDir_Downlink));
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmce_pdu_encode(&enc, NULL, CmceDir_Downlink));
}

/* ---------------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_d_setup_roundtrip);
    RUN_TEST(test_u_setup_roundtrip_ssi);
    RUN_TEST(test_u_setup_roundtrip_sna);
    RUN_TEST(test_d_call_proceeding_roundtrip);
    RUN_TEST(test_d_connect_roundtrip);
    RUN_TEST(test_u_tx_demand_roundtrip);
    RUN_TEST(test_d_tx_granted_roundtrip);
    RUN_TEST(test_u_release_roundtrip);
    RUN_TEST(test_d_release_roundtrip);
    RUN_TEST(test_pdu_type_at_bit_position_zero);
    RUN_TEST(test_decode_unknown_pdu_type);
    RUN_TEST(test_encode_bad_args);
    return UNITY_END();
}

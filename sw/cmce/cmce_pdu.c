/* sw/cmce/cmce_pdu.c — CMCE PDU encode/decode (bit-exact on-air).
 *
 * Owned by S4 (S4-sw-cmce). Locked under IF_CMCE_v1.
 *
 * ============================================================================
 *   PROVISIONAL — CMCE FIELD VALUES NOT GOLD-VERIFIED
 * ============================================================================
 *
 * Per docs/references/gold_field_values.md §"Open uncertainties" #5:
 *
 *     "Alle CMCE-PDU-Felder (D-SETUP / U-SETUP / D-CALL-PROCEEDING /
 *      D-CONNECT / U-TX-DEMAND / D-TX-GRANTED / U-RELEASE / D-RELEASE).
 *      PROVISIONAL aus bluestation-Defaults + ETSI §14.7. Nicht Gold-
 *      verifiziert. Action: Group-Call CMCE-Phase aufnehmen, bit-
 *      extrahieren, bestätigen oder korrigieren."
 *
 * The bit layouts in this file follow reference_cmce_group_call_pdus.md
 * (which itself synthesises bluestation cmce/pdus/<x>.rs + ETSI §14.7).
 * They are unit-tested for round-trip + structural correctness only.
 * Phase-G live-air capture will replace the round-trip test gate with
 * bit-exact-vs-Gold tests.
 *
 * D-NWRK-BROADCAST is the SOLE CMCE-adjacent PDU with Gold-Ref backing
 * (Burst #423 → GOLD_INFO_124 from scripts/gen_d_nwrk_broadcast.py); see
 * cmce_pdu_encode_d_nwrk_broadcast() below.
 *
 * ============================================================================
 *
 * Bit-layout reference (MSB-first throughout, matching BitBuffer):
 *
 *   D-SETUP — Clause 14.7.1.12, see ref_cmce_group_call_pdus.md §D-SETUP
 *     [ 0..  4] pdu_type             = 5 bit (=7)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 22] call_time_out        = 4 bit
 *     [23]      hook_method_selection = 1 bit
 *     [24]      simplex_duplex_selection = 1 bit
 *     [25.. 32] basic_service_information = 8 bit
 *     [33.. 34] transmission_grant   = 2 bit
 *     [35]      transmission_request_permission = 1 bit
 *     [36.. 39] call_priority        = 4 bit
 *     [40]      o-bit                = 1 bit
 *
 *   U-SETUP — Clause 14.7.2.10, see ref §U-SETUP
 *     [ 0..  4] pdu_type             = 5 bit (=7)
 *     [ 5..  8] area_selection       = 4 bit
 *     [ 9]      hook_method_selection = 1 bit
 *     [10]      simplex_duplex_selection = 1 bit
 *     [11.. 18] basic_service_information = 8 bit
 *     [19]      request_to_transmit_send_data = 1 bit
 *     [20.. 23] call_priority        = 4 bit
 *     [24.. 25] clir_control         = 2 bit
 *     [26.. 27] called_party_type_identifier = 2 bit
 *     ... (variable on PartyType)
 *
 *   D-CALL-PROCEEDING — Clause 14.7.1.2
 *     [ 0..  4] pdu_type             = 5 bit (=1)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 21] call_time_out_set_up_phase = 3 bit
 *     [22]      hook_method_selection = 1 bit
 *     [23]      simplex_duplex_selection = 1 bit
 *     [24]      o-bit                = 1 bit
 *
 *   D-CONNECT — Clause 14.7.1.4
 *     [ 0..  4] pdu_type             = 5 bit (=2)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 22] call_time_out        = 4 bit
 *     [23]      hook_method_selection = 1 bit
 *     [24]      simplex_duplex_selection = 1 bit
 *     [25.. 26] transmission_grant   = 2 bit
 *     [27]      transmission_request_permission = 1 bit
 *     [28]      call_ownership       = 1 bit
 *     [29]      o-bit                = 1 bit
 *
 *   D-TX-GRANTED — Clause 14.7.1.15
 *     [ 0..  4] pdu_type             = 5 bit (=11)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 20] transmission_grant   = 2 bit
 *     [21]      transmission_request_permission = 1 bit
 *     [22]      encryption_control   = 1 bit
 *     [23]      reserved (=0)        = 1 bit
 *     [24]      o-bit                = 1 bit
 *
 *   U-TX-DEMAND — Clause 14.7.2.12
 *     [ 0..  4] pdu_type             = 5 bit (=10)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 20] tx_demand_priority   = 2 bit
 *     [21]      encryption_control   = 1 bit
 *     [22]      reserved (=0)        = 1 bit
 *     [23]      o-bit                = 1 bit
 *
 *   U/D-RELEASE — Clauses 14.7.2.9 / 14.7.1.9
 *     [ 0..  4] pdu_type             = 5 bit (=6)
 *     [ 5.. 18] call_identifier      = 14 bit
 *     [19.. 23] disconnect_cause     = 5 bit
 *     [24]      o-bit                = 1 bit
 */

#include "tetra/cmce.h"
#include "tetra/msgbus.h"
#include "tetra/types.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal — write/read pdu_type (5 bit) at the cursor. The on-air pdu_type
 * encoding is a 5-bit field per ETSI §14.8.28 / §14.8.29 (DL/UL tables);
 * `CmcePduType` enum values match the on-air integer for the entries we
 * encode. The 0xFE / 0xFF sentinels are NOT valid on-air values; if one
 * appears at decode the result is CmcePdu_Unknown and the caller gets
 * -EPROTO.
 * ------------------------------------------------------------------------- */
static int put_pdu_type(BitBuffer *out, uint8_t v)
{
    if (v > 0x1Fu) {
        return -EINVAL;
    }
    bb_put_bits(out, (uint32_t) v, 5);
    return 0;
}

static uint8_t get_pdu_type(BitBuffer *in)
{
    return (uint8_t) bb_get_bits(in, 5);
}

/* ---------------------------------------------------------------------------
 * D-SETUP encoder — fields valid through bit 40 (mandatory portion + o-bit).
 *
 * Optional-field decoding past the o-bit is NOT implemented at this milestone
 * — see "PROVISIONAL" notice at top of file. When `pdu->optionals_present`
 * is true we still write the o-bit (=1) but no Type-2/Type-3 IEs follow;
 * downstream Phase-G capture-driven work will fill in the optional-field
 * encoder. Decoder mirrors this: o-bit is read but optional fields are
 * skipped (consumer should not rely on optional-field values until that
 * lands).
 * ------------------------------------------------------------------------- */
static int encode_d_setup(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu) return -EINVAL;
    if (p->call_time_out   > 0x0Fu)   return -EINVAL;
    if (p->call_priority   > 0x0Fu)   return -EINVAL;
    if (p->transmission_grant > 0x3u) return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_Setup);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier      & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->call_time_out        & 0x0Fu,    4);
    bb_put_bits(out, (uint32_t) (p->hook_method_selection      & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->simplex_duplex_selection   & 0x1u), 1);
    bb_put_bits(out, (uint32_t) p->basic_service_information,           8);
    bb_put_bits(out, (uint32_t) p->transmission_grant   & 0x3u,         2);
    bb_put_bits(out, (uint32_t) (p->transmission_request_permission & 0x1u), 1);
    bb_put_bits(out, (uint32_t) p->call_priority        & 0x0Fu,        4);
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u),       1);
    return 0;
}

static int decode_d_setup(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    /* pdu_type already consumed by caller. Need 36 more bits (5..40). */
    if (in_len_bits < 41u) return -EPROTO;
    p->call_identifier               = (uint16_t) bb_get_bits(in, 14);
    p->call_time_out                 = (uint8_t)  bb_get_bits(in,  4);
    p->hook_method_selection         = (uint8_t)  bb_get_bits(in,  1);
    p->simplex_duplex_selection      = (uint8_t)  bb_get_bits(in,  1);
    p->basic_service_information     = (uint8_t)  bb_get_bits(in,  8);
    p->transmission_grant            = (CmceTransmissionGrant) bb_get_bits(in, 2);
    p->transmission_request_permission = (uint8_t) bb_get_bits(in, 1);
    p->call_priority                 = (uint8_t)  bb_get_bits(in,  4);
    p->optionals_present             = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * U-SETUP encoder — covers the mandatory fields through called_party
 * (variable-width). Optional fields after called_party are NOT written.
 * ------------------------------------------------------------------------- */
static int encode_u_setup(BitBuffer *out, const CmcePdu *p)
{
    if (p->area_selection > 0x0Fu)            return -EINVAL;
    if (p->call_priority  > 0x0Fu)            return -EINVAL;
    if (p->clir_control   > 0x3u)             return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_Setup);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->area_selection                & 0x0Fu, 4);
    bb_put_bits(out, (uint32_t) (p->hook_method_selection        & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->simplex_duplex_selection     & 0x1u), 1);
    bb_put_bits(out, (uint32_t) p->basic_service_information,             8);
    bb_put_bits(out, (uint32_t) (p->request_to_transmit_send_data & 0x1u),1);
    bb_put_bits(out, (uint32_t) p->call_priority                  & 0x0Fu,4);
    bb_put_bits(out, (uint32_t) p->clir_control                   & 0x3u, 2);
    bb_put_bits(out, (uint32_t) p->called_party_type_identifier   & 0x3u, 2);

    /* Variable-width called_party. */
    switch (p->called_party_type_identifier) {
    case CmcePty_Sna:
        bb_put_bits(out,
                    (uint32_t) p->called_party_short_number_address, 8);
        break;
    case CmcePty_Ssi:
        bb_put_bits(out,
                    (uint32_t) (p->called_party_address_ssi & TETRA_SSI_MASK_24),
                    24);
        break;
    case CmcePty_Tsi:
        /* SSI[24] + extension[24] — extension comes from a separate
         * field in the higher layer; not stored in our struct yet, so
         * for the M3 test path we emit just the SSI portion. */
        bb_put_bits(out,
                    (uint32_t) (p->called_party_address_ssi & TETRA_SSI_MASK_24),
                    24);
        break;
    case CmcePty_Reserved:
    default:
        /* No body to emit — caller is responsible for not using Reserved. */
        break;
    }

    /* o-bit. */
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u), 1);
    return 0;
}

static int decode_u_setup(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    if (in_len_bits < 30u) return -EPROTO;
    p->area_selection                = (uint8_t) bb_get_bits(in, 4);
    p->hook_method_selection         = (uint8_t) bb_get_bits(in, 1);
    p->simplex_duplex_selection      = (uint8_t) bb_get_bits(in, 1);
    p->basic_service_information     = (uint8_t) bb_get_bits(in, 8);
    p->request_to_transmit_send_data = (uint8_t) bb_get_bits(in, 1);
    p->call_priority                 = (uint8_t) bb_get_bits(in, 4);
    p->clir_control                  = (uint8_t) bb_get_bits(in, 2);
    p->called_party_type_identifier  = (CmcePartyType) bb_get_bits(in, 2);

    switch (p->called_party_type_identifier) {
    case CmcePty_Sna:
        if (bb_remaining(in) < 8u + 1u) return -EPROTO;
        p->called_party_short_number_address = (uint8_t) bb_get_bits(in, 8);
        break;
    case CmcePty_Ssi:
    case CmcePty_Tsi:
        if (bb_remaining(in) < 24u + 1u) return -EPROTO;
        p->called_party_address_ssi = bb_get_bits(in, 24) & TETRA_SSI_MASK_24;
        break;
    case CmcePty_Reserved:
    default:
        break;
    }
    p->optionals_present = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * D-CALL-PROCEEDING — 25 bits mandatory + o-bit.
 * ------------------------------------------------------------------------- */
static int encode_d_call_proceeding(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu)              return -EINVAL;
    if (p->call_time_out_set_up_phase > 0x07u)     return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_CallProceeding);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->call_time_out_set_up_phase & 0x07u, 3);
    bb_put_bits(out, (uint32_t) (p->hook_method_selection    & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->simplex_duplex_selection & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u),     1);
    return 0;
}

static int decode_d_call_proceeding(BitBuffer *in, CmcePdu *p,
                                    uint16_t in_len_bits)
{
    if (in_len_bits < 25u) return -EPROTO;
    p->call_identifier                = (uint16_t) bb_get_bits(in, 14);
    p->call_time_out_set_up_phase     = (uint8_t)  bb_get_bits(in,  3);
    p->hook_method_selection          = (uint8_t)  bb_get_bits(in,  1);
    p->simplex_duplex_selection       = (uint8_t)  bb_get_bits(in,  1);
    p->optionals_present              = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * D-CONNECT — 30 bits mandatory + o-bit.
 * ------------------------------------------------------------------------- */
static int encode_d_connect(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu)         return -EINVAL;
    if (p->call_time_out   > 0x0Fu)           return -EINVAL;
    if (p->transmission_grant > 0x3u)         return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_Connect);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier  & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->call_time_out    & 0x0Fu,    4);
    bb_put_bits(out, (uint32_t) (p->hook_method_selection    & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->simplex_duplex_selection & 0x1u), 1);
    bb_put_bits(out, (uint32_t) p->transmission_grant & 0x3u,   2);
    bb_put_bits(out, (uint32_t) (p->transmission_request_permission & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->call_ownership   & 0x1u),   1);
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u), 1);
    return 0;
}

static int decode_d_connect(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    if (in_len_bits < 30u) return -EPROTO;
    p->call_identifier               = (uint16_t) bb_get_bits(in, 14);
    p->call_time_out                 = (uint8_t)  bb_get_bits(in,  4);
    p->hook_method_selection         = (uint8_t)  bb_get_bits(in,  1);
    p->simplex_duplex_selection      = (uint8_t)  bb_get_bits(in,  1);
    p->transmission_grant            = (CmceTransmissionGrant) bb_get_bits(in, 2);
    p->transmission_request_permission = (uint8_t) bb_get_bits(in, 1);
    p->call_ownership                = (uint8_t)  bb_get_bits(in,  1);
    p->optionals_present             = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * D-TX-GRANTED — 25 bits mandatory + o-bit.
 * ------------------------------------------------------------------------- */
static int encode_d_tx_granted(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu)  return -EINVAL;
    if (p->transmission_grant > 0x3u)  return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_TxGranted);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->transmission_grant & 0x3u, 2);
    bb_put_bits(out, (uint32_t) (p->transmission_request_permission & 0x1u), 1);
    bb_put_bits(out, (uint32_t) (p->encryption_control & 0x1u), 1);
    bb_put_bits(out, 0u, 1);  /* reserved = 0 per §14.7.1.15 */
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u), 1);
    return 0;
}

static int decode_d_tx_granted(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    if (in_len_bits < 25u) return -EPROTO;
    p->call_identifier               = (uint16_t) bb_get_bits(in, 14);
    p->transmission_grant            = (CmceTransmissionGrant) bb_get_bits(in, 2);
    p->transmission_request_permission = (uint8_t) bb_get_bits(in, 1);
    p->encryption_control            = (uint8_t)  bb_get_bits(in, 1);
    (void) bb_get_bits(in, 1);  /* reserved */
    p->optionals_present             = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * U-TX-DEMAND — 24 bits mandatory + o-bit.
 * ------------------------------------------------------------------------- */
static int encode_u_tx_demand(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu)  return -EINVAL;
    if (p->tx_demand_priority > 0x3u)  return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_TxDemand);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->tx_demand_priority & 0x3u, 2);
    bb_put_bits(out, (uint32_t) (p->encryption_control & 0x1u), 1);
    bb_put_bits(out, 0u, 1); /* reserved = 0 */
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u), 1);
    return 0;
}

static int decode_u_tx_demand(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    if (in_len_bits < 24u) return -EPROTO;
    p->call_identifier            = (uint16_t) bb_get_bits(in, 14);
    p->tx_demand_priority         = (uint8_t)  bb_get_bits(in,  2);
    p->encryption_control         = (uint8_t)  bb_get_bits(in,  1);
    (void) bb_get_bits(in, 1);  /* reserved */
    p->optionals_present          = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * U/D-RELEASE — 25 bits mandatory + o-bit. Same layout both directions.
 * ------------------------------------------------------------------------- */
static int encode_release(BitBuffer *out, const CmcePdu *p)
{
    if (p->call_identifier > 0x3FFFu)            return -EINVAL;
    if ((unsigned) p->disconnect_cause > 0x1Fu)  return -EINVAL;

    int rc = put_pdu_type(out, (uint8_t) CmcePdu_Release);
    if (rc < 0) return rc;
    bb_put_bits(out, (uint32_t) p->call_identifier & 0x3FFFu, 14);
    bb_put_bits(out, (uint32_t) p->disconnect_cause & 0x1Fu, 5);
    bb_put_bits(out, (uint32_t) (p->optionals_present ? 1u : 0u), 1);
    return 0;
}

static int decode_release(BitBuffer *in, CmcePdu *p, uint16_t in_len_bits)
{
    if (in_len_bits < 25u) return -EPROTO;
    p->call_identifier   = (uint16_t) bb_get_bits(in, 14);
    p->disconnect_cause  = (CmceDisconnectCause) bb_get_bits(in, 5);
    p->optionals_present = (bb_get_bits(in, 1) != 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API — generic encode/decode dispatcher.
 * ------------------------------------------------------------------------- */
int cmce_pdu_encode(BitBuffer *out, const CmcePdu *pdu, CmceDirection dir)
{
    if (out == NULL || pdu == NULL) return -EINVAL;

    const size_t start = bb_pos_bits(out);
    int rc = -EINVAL;

    switch (pdu->pdu_type) {
    case CmcePdu_Setup:
        rc = (dir == CmceDir_Downlink) ? encode_d_setup(out, pdu)
                                       : encode_u_setup(out, pdu);
        break;
    case CmcePdu_CallProceeding:
        if (dir != CmceDir_Downlink) return -EINVAL;
        rc = encode_d_call_proceeding(out, pdu);
        break;
    case CmcePdu_Connect:
        if (dir != CmceDir_Downlink) return -EINVAL;
        rc = encode_d_connect(out, pdu);
        break;
    case CmcePdu_TxDemand:
        if (dir != CmceDir_Uplink) return -EINVAL;
        rc = encode_u_tx_demand(out, pdu);
        break;
    case CmcePdu_TxGranted:
        if (dir != CmceDir_Downlink) return -EINVAL;
        rc = encode_d_tx_granted(out, pdu);
        break;
    case CmcePdu_Release:
        rc = encode_release(out, pdu);
        break;
    case CmcePdu_NwrkBroadcast:
        /* Not via this entry point — see cmce_pdu_encode_d_nwrk_broadcast. */
        return -EINVAL;
    default:
        return -EINVAL;
    }
    if (rc < 0) return rc;

    return (int) (bb_pos_bits(out) - start);
}

int cmce_pdu_decode(BitBuffer *in, CmcePdu *out, CmceDirection dir,
                    uint16_t in_len_bits)
{
    if (in == NULL || out == NULL) return -EINVAL;
    if (in_len_bits < 5u)           return -EPROTO;

    memset(out, 0, sizeof(*out));
    const uint8_t raw = get_pdu_type(in);
    int rc = -EPROTO;

    switch (raw) {
    case (uint8_t) CmcePdu_Setup:
        out->pdu_type = CmcePdu_Setup;
        rc = (dir == CmceDir_Downlink)
             ? decode_d_setup(in, out, in_len_bits)
             : decode_u_setup(in, out, in_len_bits);
        break;
    case (uint8_t) CmcePdu_CallProceeding:
        if (dir != CmceDir_Downlink) return -EPROTO;
        out->pdu_type = CmcePdu_CallProceeding;
        rc = decode_d_call_proceeding(in, out, in_len_bits);
        break;
    case (uint8_t) CmcePdu_Connect:
        if (dir != CmceDir_Downlink) return -EPROTO;
        out->pdu_type = CmcePdu_Connect;
        rc = decode_d_connect(in, out, in_len_bits);
        break;
    case (uint8_t) CmcePdu_TxDemand:
        if (dir != CmceDir_Uplink) return -EPROTO;
        out->pdu_type = CmcePdu_TxDemand;
        rc = decode_u_tx_demand(in, out, in_len_bits);
        break;
    case (uint8_t) CmcePdu_TxGranted:
        if (dir != CmceDir_Downlink) return -EPROTO;
        out->pdu_type = CmcePdu_TxGranted;
        rc = decode_d_tx_granted(in, out, in_len_bits);
        break;
    case (uint8_t) CmcePdu_Release:
        out->pdu_type = CmcePdu_Release;
        rc = decode_release(in, out, in_len_bits);
        break;
    default:
        out->pdu_type = CmcePdu_Unknown;
        return -EPROTO;
    }
    if (rc < 0) return rc;

    out->encoded_len_bits = in_len_bits;
    return 0;
}

/* ---------------------------------------------------------------------------
 * D-NWRK-BROADCAST — Gold-Ref-backed (Burst #423).
 *
 * Body layout per docs/references/gold_field_values.md §"D-NWRK-BCAST Body
 * Felder":
 *
 *   [0..15] cell_re_select_parameters   (16 bit, Type 1)  — Gold = 0x5655
 *   [16..17] cell_load_ca               ( 2 bit, Type 1)  — Gold = 0
 *   [18]    o-bit                       ( 1 bit)          — Gold:open uncertainty
 *   ... (Type-2/Type-3 if o-bit=1)
 *
 * Conservative encoder default per gold_field_values.md
 * §"Konservativer Default für unseren Encoder": o-bit=0 ⇒ no Type-2 fields.
 * Reproduces 19 info bits for the D-NWRK-BCAST body proper.
 *
 * The MAC-RESOURCE wrapper (51 bits per gold layout: pdu_type[2] + fill[1]
 * + pog[1] + enc[2] + ra[1] + LI[6] + addr_type[3] + SSI[24] + 11 bits of
 * bluestation-conflict region; see gold_field_values.md §"KONFLIKT Gold ↔
 * Bluestation") and the LLC + MLE-disc + MLE-prim (10 bits) are NOT the
 * CMCE entity's responsibility. They live in lower-layer encoders (LLC for
 * BL-UDATA wrap, UMAC for the MAC-RESOURCE). The D-NWRK-BCAST emitter
 * here produces only the MLE D-NWRK-BCAST body bits, which is what the
 * upper-layer / TleSap interface carries.
 * ------------------------------------------------------------------------- */
int cmce_pdu_encode_d_nwrk_broadcast(BitBuffer *out, const CmcePdu *pdu)
{
    if (out == NULL || pdu == NULL) return -EINVAL;
    if (pdu->nwrk_cell_load_ca > 0x3u) return -EINVAL;

    /* Conservative default — refuse to emit guessed optional bits. The
     * open uncertainty in gold_field_values.md §"Open uncertainties" #2
     * means we do not know the TNT bit-allocation in Gold #423. Hard-stop
     * if the caller asks for optionals_present=true. */
    if (pdu->optionals_present) {
        return -ENOTSUP;
    }

    const size_t start = bb_pos_bits(out);
    bb_put_bits(out, (uint32_t) pdu->nwrk_cell_re_select_parameters, 16);
    bb_put_bits(out, (uint32_t) pdu->nwrk_cell_load_ca & 0x3u,        2);
    bb_put_bits(out, 0u,                                              1);  /* o-bit = 0 */
    return (int) (bb_pos_bits(out) - start);
}

int cmce_pdu_decode_d_nwrk_broadcast(BitBuffer *in, CmcePdu *out,
                                     uint16_t in_len_bits)
{
    if (in == NULL || out == NULL) return -EINVAL;
    if (in_len_bits < 19u)         return -EPROTO;

    /* Do NOT memset — the NWRK decoder is composable with a calling-side
     * header decoder that may have already filled in pdu_type. */
    out->pdu_type = CmcePdu_NwrkBroadcast;
    out->nwrk_cell_re_select_parameters = (uint16_t) bb_get_bits(in, 16);
    out->nwrk_cell_load_ca              = (uint8_t)  bb_get_bits(in,  2);
    out->optionals_present              = (bb_get_bits(in, 1) != 0);
    out->encoded_len_bits               = 19;
    return 19;
}

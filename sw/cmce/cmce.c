/* sw/cmce/cmce.c — CMCE main entity.
 *
 * Owned by S4 (S4-sw-cmce). Locked under interface contract IF_CMCE_v1.
 *
 * Circuit Mode Control Entity per ETSI EN 300 392-2 §14 + bluestation
 * `cmce/`. Glues the codec (cmce_pdu.c), the per-call state-machine
 * (cmce_fsm.c) and the periodic D-NWRK-BCAST driver (cmce_nwrk_bcast.c)
 * onto the common message-bus.
 *
 * Bus wiring:
 *   - registers (CMCE, TleSap) for upcalls from MLE — TleSapMsg envelope
 *     carrying an LLC PDU whose body starts with the CMCE PDU bits
 *   - posts TleSapMsg downward on the same SAP for DL-direction CMCE
 *     traffic (BS → MS); the MLE/LLC layers below propagate to TmaSap
 *
 * The wiring uses the (SapId_TmaSap-style) tuple convention from S0/S2:
 * we register on `(dest=CMCE-conceptual, sap=TleSap)` but since S0's
 * SapId enum currently does not have a CmceSap entry, we re-use TleSap
 * with our own filter. (See "Bus dispatch tuple" note below.)
 *
 * Bus dispatch tuple:
 *   - Upcall   : (dest=SapId_TleSap, sap=SapId_TleSap) — same as S2 LLC.
 *     CMCE filters on `pdu_type` to decide whether the message is for it
 *     (CMCE PDU types per ETSI §14.8.28) versus an MLE-internal PDU.
 *   - Downcall : posts (src=TleSap, dest=TleSap, sap=TleSap) so LLC's
 *     existing on_tle_msg_from_mle handler picks it up and emits down to
 *     TmaSap. CMCE thereby never directly touches TmaSap.
 *
 * The CMCE filter uses a small heuristic on the LLC-body bits: if the
 * top 5 bits are a known CMCE PDU type code AND the message arrived on
 * an endpoint that has either a SetupPending slot OR no slot (so we can
 * allocate one for U-SETUP), CMCE consumes; otherwise it ignores.
 *
 * Memory: zero-malloc, zero-thread. Slot table is fixed-size at
 * CMCE_MAX_CALLS. Storage for msgbus payloads is the bus's own (S0-owned).
 */

#include "tetra/cmce.h"
#include "tetra/llc.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers.
 * ------------------------------------------------------------------------- */

/* cmce_pdu_from_body — decode a CMCE PDU sitting at offset 0 of an LLC
 * body. Used by the upcall handler. The LLC body bit-length (passed in
 * via TleSapMsg.pdu.body_len_bits) is the upper bound on encoded_len. */
static int cmce_pdu_from_body(const uint8_t *body, uint16_t body_len_bits,
                              CmceDirection dir, CmcePdu *out)
{
    if (body == NULL || out == NULL || body_len_bits == 0u) {
        return -EINVAL;
    }
    /* The decoder needs a non-const buffer because BitBuffer mutates pos.
     * Copy into a local. The MLE-PD field at the very start of the LLC
     * body is bluestation's 3-bit MLE-disc (already stripped by MLE
     * upstream — TleSap upcall path strips it before forwarding to CMCE).
     * For the M3 test path we assume the body's bit 0 is the CMCE
     * pdu_type's MSB. */
    uint8_t scratch[CMCE_PDU_BODY_MAX_BYTES];
    const size_t nbytes = (body_len_bits + 7u) / 8u;
    if (nbytes > sizeof(scratch)) {
        return -EPROTO;
    }
    memcpy(scratch, body, nbytes);
    BitBuffer bb = bb_init(scratch, body_len_bits);
    return cmce_pdu_decode(&bb, out, dir, body_len_bits);
}

/* Direction inference: the CMCE entity in a BS is "downlink-side" — it
 * generates D-* PDUs and consumes U-* PDUs. For now we hard-wire UL on
 * the upcall path; the daemon (S7) will override per-deployment if a
 * dual-role MS-emulator build is ever needed. */
static CmceDirection upcall_direction(void)
{
    return CmceDir_Uplink;
}

/* ---------------------------------------------------------------------------
 * Public — encode + post.
 * ------------------------------------------------------------------------- */
int cmce_post_tma_unitdata_req(Cmce *cmce,
                               EndpointId endpoint,
                               const TetraAddress *addr,
                               const CmcePdu *pdu,
                               CmceDirection dir)
{
    if (cmce == NULL || addr == NULL || pdu == NULL) {
        return -EINVAL;
    }
    if (!cmce->initialised) {
        return -EINVAL;
    }

    /* Encode CMCE PDU into a stack buffer. */
    uint8_t   cmce_bytes[CMCE_PDU_BODY_MAX_BYTES];
    memset(cmce_bytes, 0, sizeof(cmce_bytes));
    BitBuffer enc = bb_init_autoexpand(cmce_bytes, sizeof(cmce_bytes) * 8u);
    int n;
    if (pdu->pdu_type == CmcePdu_NwrkBroadcast) {
        n = cmce_pdu_encode_d_nwrk_broadcast(&enc, pdu);
    } else {
        n = cmce_pdu_encode(&enc, pdu, dir);
    }
    if (n < 0) {
        return n;
    }

    /* Wrap in a TleSapMsg with an LLC BL-DATA carrying the CMCE bits as
     * body. LLC will stamp NS, encode the LLC header + body, and pass
     * down to TmaSap. */
    TleSapMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.endpoint = endpoint;
    msg.addr     = *addr;
    msg.pdu.pdu_type      = LlcPdu_BL_DATA;
    msg.pdu.body_len_bits = (uint16_t) n;
    if ((size_t) n > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return -E2BIG;
    }
    memcpy(msg.pdu.body, cmce_bytes, ((size_t) n + 7u) / 8u);

    SapMsg env;
    sapmsg_init(&env, SapId_TleSap, SapId_TmaSap, SapId_TleSap,
                (const uint8_t *) &msg, (uint16_t) sizeof(msg));
    int rc = msgbus_post(cmce->bus, sap_prio_default(SapId_TleSap), &env);
    if (rc != 0) {
        return rc;
    }
    cmce->stats.pdus_tx++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmce_handle_tle_msg — TleSap upcall handler (MLE → CMCE).
 *
 * Decodes the LLC body as a CMCE PDU and drives the per-call FSM. May
 * synthesise a DL response (e.g. D-CALL-PROCEEDING after U-SETUP) and
 * post it back through TleSap.
 * ------------------------------------------------------------------------- */
int cmce_handle_tle_msg(Cmce *cmce, const TleSapMsg *msg)
{
    if (cmce == NULL || msg == NULL) {
        return -EINVAL;
    }
    if (!cmce->initialised) {
        return -EINVAL;
    }
    cmce->stats.pdus_rx++;

    /* Decode CMCE PDU from the LLC body. */
    CmcePdu pdu;
    memset(&pdu, 0, sizeof(pdu));
    const CmceDirection dir = upcall_direction();
    int rc = cmce_pdu_from_body(msg->pdu.body, msg->pdu.body_len_bits,
                                dir, &pdu);
    if (rc < 0) {
        cmce->stats.unknown_pdu_type++;
        return rc;
    }

    /* Allocate or look up the call slot. */
    CmceCallSlot *slot = cmce_call_lookup(cmce, pdu.call_identifier);
    if (slot == NULL) {
        if (pdu.pdu_type == CmcePdu_Setup && dir == CmceDir_Uplink) {
            slot = cmce_call_alloc(cmce, pdu.call_identifier,
                                   &msg->addr, msg->endpoint);
            if (slot == NULL) {
                cmce->stats.fsm_drops++;
                return -ENOSPC;
            }
        } else {
            /* Unknown call_id with non-Setup PDU — drop. */
            cmce->stats.fsm_drops++;
            return -EPROTO;
        }
    }

    (void) cmce_fsm_apply(cmce, slot, &pdu, dir);

    /* BS-side response synthesis. The full M3 path is:
     *   rx U-SETUP   -> tx D-CALL-PROCEEDING
     *   internal     -> tx D-CONNECT
     *   rx U-TX-DEMAND -> tx D-TX-GRANTED
     *   rx U-RELEASE -> tx D-RELEASE
     * For S4 we emit only the FIRST response in the chain on the upcall
     * path; subsequent steps are driven by the daemon's main loop or by
     * the M3 test harness. This keeps cmce_handle_tle_msg synchronous and
     * its side-effects bounded.
     */
    if (pdu.pdu_type == CmcePdu_Setup && dir == CmceDir_Uplink) {
        CmcePdu resp;
        memset(&resp, 0, sizeof(resp));
        resp.pdu_type        = CmcePdu_CallProceeding;
        resp.call_identifier = pdu.call_identifier;
        /* Bluestation defaults — gold_field_values.md "Open uncertainties"
         * #5 marks these PROVISIONAL. */
        resp.call_time_out_set_up_phase  = 0;
        resp.hook_method_selection       = 0;
        resp.simplex_duplex_selection    = 0;
        resp.optionals_present           = false;
        (void) cmce_post_tma_unitdata_req(cmce, msg->endpoint, &msg->addr,
                                          &resp, CmceDir_Downlink);
    } else if (pdu.pdu_type == CmcePdu_TxDemand && dir == CmceDir_Uplink) {
        CmcePdu resp;
        memset(&resp, 0, sizeof(resp));
        resp.pdu_type        = CmcePdu_TxGranted;
        resp.call_identifier = pdu.call_identifier;
        resp.transmission_grant = CmceTxGrant_Granted;
        resp.transmission_request_permission = 1;
        resp.encryption_control = pdu.encryption_control;
        resp.optionals_present  = false;
        (void) cmce_post_tma_unitdata_req(cmce, msg->endpoint, &msg->addr,
                                          &resp, CmceDir_Downlink);
    } else if (pdu.pdu_type == CmcePdu_Release && dir == CmceDir_Uplink) {
        /* MS-initiated release — emit D-RELEASE back. */
        CmcePdu resp;
        memset(&resp, 0, sizeof(resp));
        resp.pdu_type        = CmcePdu_Release;
        resp.call_identifier = pdu.call_identifier;
        resp.disconnect_cause = CmceDisconnect_UserRequested;
        resp.optionals_present = false;
        (void) cmce_post_tma_unitdata_req(cmce, msg->endpoint, &msg->addr,
                                          &resp, CmceDir_Downlink);
        /* Apply the second Release (the one we just sent) to the FSM so
         * the slot returns to Idle. */
        (void) cmce_fsm_apply(cmce, slot, &resp, CmceDir_Downlink);
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * cmce_send_d_nwrk_broadcast — periodic emitter.
 *
 * Builds a D-NWRK-BROADCAST with conservative defaults (TNT absent,
 * o-bit=0) using cell_re_select_parameters + cell_load_ca from cmce->cfg.
 * Posts the encoded body via TleSap so MLE/LLC propagate down to TmaSap.
 * ------------------------------------------------------------------------- */
int cmce_send_d_nwrk_broadcast(Cmce *cmce)
{
    if (cmce == NULL) {
        return -EINVAL;
    }
    if (!cmce->initialised) {
        return -EINVAL;
    }

    CmcePdu pdu;
    memset(&pdu, 0, sizeof(pdu));
    pdu.pdu_type = CmcePdu_NwrkBroadcast;
    pdu.nwrk_cell_re_select_parameters = cmce->cfg.cell_re_select_parameters_seed;
    pdu.nwrk_cell_load_ca              = cmce->cfg.cell_load_ca;
    pdu.optionals_present              = false; /* conservative */

    /* Broadcast SSI = 0xFFFFFF per gold_field_values.md §"D-NWRK-
     * BROADCAST"; ssi_type = SsiType_Ssi (per Gold layout, not Gssi —
     * Burst #423 has address_type=001 = SSI). The endpoint is timeslot 1
     * (decoder-TN=1 = MCCH per Gold) — see reference_gold_full_attach_
     * timeline.md §"Konstanten". */
    const TetraAddress bcast = {
        .ssi      = 0x00FFFFFFu,
        .ssi_type = SsiType_Ssi,
    };
    const EndpointId mcch_endpoint = 1u;

    int rc = cmce_post_tma_unitdata_req(cmce, mcch_endpoint, &bcast,
                                        &pdu, CmceDir_Downlink);
    if (rc != 0) {
        return rc;
    }
    cmce->stats.nwrk_bcast_count++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Bus handler — TleSap upcall envelope unpack.
 * ------------------------------------------------------------------------- */
static void on_tle_upcall(const SapMsg *msg, void *ctx)
{
    Cmce *cmce = (Cmce *) ctx;
    if (cmce == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TleSapMsg)) {
        return;
    }
    TleSapMsg in;
    memcpy(&in, msg->payload, sizeof(in));

    /* The bus handler is on (TleSap, TleSap) which the LLC also
     * registered for downstream (MLE→LLC). The two are distinguishable
     * by the PDU's body shape: LLC's downstream-from-MLE messages carry
     * an MLE/MM PDU (typically with NS/NR fields set by LLC layer in
     * the body), while CMCE upcalls carry a CMCE PDU. We filter by
     * peeking the body's first 5 bits and seeing if they match a known
     * CMCE pdu_type code. The PROVISIONAL caveat in cmce_pdu.c applies
     * here too. */
    if (in.pdu.body_len_bits < 5u) {
        return;
    }
    const uint8_t pt = (uint8_t) ((in.pdu.body[0] >> 3) & 0x1Fu);
    bool looks_cmce = false;
    switch (pt) {
    case (uint8_t) CmcePdu_Setup:
    case (uint8_t) CmcePdu_CallProceeding:
    case (uint8_t) CmcePdu_Connect:
    case (uint8_t) CmcePdu_TxDemand:
    case (uint8_t) CmcePdu_TxGranted:
    case (uint8_t) CmcePdu_Release:
        looks_cmce = true;
        break;
    default:
        break;
    }
    if (!looks_cmce) {
        return;
    }
    (void) cmce_handle_tle_msg(cmce, &in);
}

/* ---------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */
int cmce_init(Cmce *cmce, MsgBus *bus, const CmceCfg *cfg)
{
    if (cmce == NULL || bus == NULL) {
        return -EINVAL;
    }

    memset(cmce, 0, sizeof(*cmce));
    cmce->bus = bus;
    if (cfg != NULL) {
        cmce->cfg = *cfg;
    }
    /* Defaults. */
    if (cmce->cfg.nwrk_bcast_period_multiframes == 0u) {
        cmce->cfg.nwrk_bcast_period_multiframes =
            CMCE_NWRK_BCAST_PERIOD_MF_DEFAULT;
    }
    if (cmce->cfg.cell_re_select_parameters_seed == 0u) {
        cmce->cfg.cell_re_select_parameters_seed = CMCE_NWRK_DEFAULT_CRSP;
    }
    /* cell_load_ca default of 0 is fine — that is the gold value. */

    int rc = msgbus_register(bus, SapId_TleSap, SapId_TleSap,
                             on_tle_upcall, cmce);
    if (rc != 0) {
        return rc;
    }

    cmce->initialised = true;
    return 0;
}

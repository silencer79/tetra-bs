/* sw/cmce/cmce_fsm.c — CMCE per-call state machine.
 *
 * Owned by S4 (S4-sw-cmce). Locked under IF_CMCE_v1.
 *
 * Per-call_id state machine. CMCE_MAX_CALLS slots, each tracks one active
 * call_identifier. Allocation is FIFO over a free-list maintained inline
 * in the slot array (no separate data structure).
 *
 * State transitions per
 * docs/references/reference_cmce_group_call_pdus.md §"Sequenz-Diagramme"
 * and ETSI EN 300 392-2 §14.6.
 *
 *   IDLE
 *     | rx U-SETUP (UL) OR explicit BS-trigger
 *     v
 *   SETUP_PENDING
 *     | tx D-CALL-PROCEEDING
 *     v
 *   PROCEEDING
 *     | tx D-CONNECT
 *     v
 *   CONNECTED
 *     | rx U-TX-DEMAND (UL)
 *     v
 *   TX_GRANTED
 *     | rx U-TX-CEASED (UL) — re-uses U-Release wire-type or, more often,
 *     |   a separate U-TX-CEASED PDU (not in S4-scope; we treat it the
 *     |   same as a Release for the M3 path).
 *     v
 *   CONNECTED
 *     | tx U-RELEASE OR rx D-RELEASE
 *     v
 *   RELEASING
 *     | rx release-ack OR timeout
 *     v
 *   IDLE  (slot freed)
 *
 * Surprising transitions:
 *   - U-SETUP from a peer with an *existing* call_identifier is treated
 *     as an error and ignored (FSM drop) — bluestation behaviour. The
 *     real BS would re-use the slot but our FSM is conservative until
 *     the Gold-Ref Group-Call capture clarifies behaviour.
 *   - D-RELEASE in CONNECTED and TX_GRANTED both drive to RELEASING; we
 *     do NOT distinguish the two in the FSM. The voice-relay teardown
 *     is a UMAC concern (TmdSap), not CMCE.
 */

#include "tetra/cmce.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Slot helpers.
 * ------------------------------------------------------------------------- */

CmceCallSlot *cmce_call_lookup(Cmce *cmce, uint16_t call_identifier)
{
    if (cmce == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CMCE_MAX_CALLS; ++i) {
        if (cmce->calls[i].in_use &&
            cmce->calls[i].call_identifier == call_identifier) {
            return &cmce->calls[i];
        }
    }
    return NULL;
}

CmceCallSlot *cmce_call_alloc(Cmce *cmce, uint16_t call_identifier,
                              const TetraAddress *peer, EndpointId endpoint)
{
    if (cmce == NULL) {
        return NULL;
    }
    /* Reject duplicate-id allocation outright — caller should look up
     * first. The FSM-drop path bumps stats.fsm_drops to surface this. */
    if (cmce_call_lookup(cmce, call_identifier) != NULL) {
        cmce->stats.fsm_drops++;
        return NULL;
    }
    for (size_t i = 0; i < CMCE_MAX_CALLS; ++i) {
        if (!cmce->calls[i].in_use) {
            CmceCallSlot *s = &cmce->calls[i];
            memset(s, 0, sizeof(*s));
            s->in_use          = true;
            s->call_identifier = call_identifier;
            s->state           = CmceCall_SetupPending;
            if (peer != NULL) {
                s->peer_addr   = *peer;
            }
            s->endpoint        = endpoint;
            s->retry_count     = 0;
            return s;
        }
    }
    return NULL;
}

void cmce_call_release(Cmce *cmce, CmceCallSlot *slot)
{
    if (cmce == NULL || slot == NULL) {
        return;
    }
    slot->in_use          = false;
    slot->state           = CmceCall_Idle;
    slot->call_identifier = 0;
    slot->retry_count     = 0;
}

/* ---------------------------------------------------------------------------
 * cmce_fsm_apply — drive the FSM for an incoming PDU.
 *
 * Called by cmce.c after the PDU has been decoded. Updates `slot->state`
 * in-place and returns the new state. Slot may be released (state = Idle
 * + in_use = false) on RELEASE-path completion.
 *
 * The CMCE is "responder-style" for BS-side: it reacts to UL PDUs and
 * emits DL responses via the parent cmce.c layer. The FSM does NOT itself
 * post msgbus messages — that is cmce.c's job — so this function is
 * synchronous and side-effect-free except for state mutation + counters.
 * ------------------------------------------------------------------------- */
CmceCallState cmce_fsm_apply(Cmce *cmce, CmceCallSlot *slot,
                             const CmcePdu *pdu, CmceDirection dir)
{
    if (cmce == NULL || slot == NULL || pdu == NULL || !slot->in_use) {
        if (cmce != NULL) {
            cmce->stats.fsm_drops++;
        }
        return CmceCall_Idle;
    }

    /* Per-PDU-type transitions. Order matches the typical M3 group-call
     * sequence as documented in reference_cmce_group_call_pdus.md
     * §"Sequenz-Diagramme — Group-Call MS-Initiated (PTT)". */
    switch (pdu->pdu_type) {
    case CmcePdu_Setup:
        if (dir == CmceDir_Uplink) {
            /* MS sent U-SETUP. The slot may already be SetupPending if
             * we just allocated it in cmce_handle_tle_msg(). Otherwise
             * this is a re-transmit — count as drop. */
            if (slot->state != CmceCall_SetupPending) {
                cmce->stats.fsm_drops++;
            }
            cmce->stats.setup_count++;
            slot->state = CmceCall_SetupPending;
        } else {
            /* BS sent D-SETUP — slot moves to SetupPending awaiting
             * U-CONNECT (not in M3 path) or D-CALL-PROCEEDING locally. */
            slot->state = CmceCall_SetupPending;
            cmce->stats.setup_count++;
        }
        break;

    case CmcePdu_CallProceeding:
        if (slot->state == CmceCall_SetupPending) {
            slot->state = CmceCall_Proceeding;
        } else {
            cmce->stats.fsm_drops++;
        }
        break;

    case CmcePdu_Connect:
        if (slot->state == CmceCall_Proceeding ||
            slot->state == CmceCall_SetupPending) {
            slot->state = CmceCall_Connected;
            cmce->stats.connect_count++;
        } else {
            cmce->stats.fsm_drops++;
        }
        break;

    case CmcePdu_TxDemand:
        /* MS asked for the channel — transition only if we were
         * Connected. Out-of-state TxDemand is a drop. */
        if (slot->state == CmceCall_Connected) {
            slot->state = CmceCall_TxGranted;
            cmce->stats.tx_grant_count++;
        } else {
            cmce->stats.fsm_drops++;
        }
        break;

    case CmcePdu_TxGranted:
        /* BS confirmed the grant. From the BS's PoV, this is *our* Tx;
         * from the MS's PoV this is what we tell them. Either way, the
         * slot moves to TxGranted from Connected. */
        if (slot->state == CmceCall_Connected ||
            slot->state == CmceCall_TxGranted) {
            slot->state = CmceCall_TxGranted;
            cmce->stats.tx_grant_count++;
        } else {
            cmce->stats.fsm_drops++;
        }
        break;

    case CmcePdu_Release:
        /* Release from any active state moves to Releasing. A second
         * Release closes the slot. */
        if (slot->state == CmceCall_Releasing) {
            cmce_call_release(cmce, slot);
            cmce->stats.release_count++;
            return CmceCall_Idle;
        }
        if (slot->state != CmceCall_Idle) {
            slot->state = CmceCall_Releasing;
            cmce->stats.release_count++;
        } else {
            cmce->stats.fsm_drops++;
        }
        break;

    default:
        /* D-NWRK-BCAST / Unknown — does not drive the per-call FSM. */
        cmce->stats.fsm_drops++;
        break;
    }

    return slot->state;
}

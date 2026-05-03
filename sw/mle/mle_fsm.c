/* sw/mle/mle_fsm.c — MLE Registration FSM.
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MLE_v1.
 *
 * State machine per docs/MIGRATION_PLAN.md §S3:
 *   IDLE -> ATTACH_PENDING -> REGISTERED -> GROUP_ATTACH -> REGISTERED
 *                                       -> DETACHING -> IDLE
 *
 * Transitions (event):
 *   IDLE          + DemandReceived     -> ATTACH_PENDING
 *   ATTACH_PENDING+ AcceptSent         -> REGISTERED   (accept emitted by MM)
 *   REGISTERED    + GrpDemandReceived  -> GROUP_ATTACH
 *   GROUP_ATTACH  + GrpAckSent         -> REGISTERED
 *   REGISTERED    + DetachReceived     -> DETACHING
 *   DETACHING     + DetachComplete     -> IDLE
 *   any           + AcceptAcked        -> no state change (just bookkeeping)
 *
 * Test gate: tb/sw/mle/test_mle_fsm.c walks IDLE -> ... -> REGISTERED.
 */
#include "tetra/mle.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

MleState mle_fsm_step(MleSession *s, MleEvt e)
{
    if (s == NULL) {
        return MleState_Idle;
    }
    const MleState old = s->state;

    switch (old) {
    case MleState_Idle:
        if (e == MleEvt_DemandReceived) {
            s->state = MleState_AttachPending;
        }
        break;
    case MleState_AttachPending:
        if (e == MleEvt_AcceptSent) {
            s->state = MleState_Registered;
        } else if (e == MleEvt_DetachReceived) {
            s->state = MleState_Detaching;
        }
        break;
    case MleState_Registered:
        if (e == MleEvt_GrpDemandReceived) {
            s->state = MleState_GroupAttach;
        } else if (e == MleEvt_DetachReceived) {
            s->state = MleState_Detaching;
        } else if (e == MleEvt_DemandReceived) {
            /* Re-attach without intermediate detach — restart attach. */
            s->state = MleState_AttachPending;
        }
        break;
    case MleState_GroupAttach:
        if (e == MleEvt_GrpAckSent) {
            s->state = MleState_Registered;
        } else if (e == MleEvt_DetachReceived) {
            s->state = MleState_Detaching;
        }
        break;
    case MleState_Detaching:
        if (e == MleEvt_DetachComplete) {
            s->state = MleState_Idle;
        }
        break;
    default:
        s->state = MleState_Idle;
        break;
    }
    return s->state;
}

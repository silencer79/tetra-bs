/* sw/mle/include/tetra/mle.h — MLE (Mobility Link Entity) public API.
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MLE_v1.
 *
 * MLE per ETSI EN 300 392-2 §18 + bluestation `mle/`. Handles:
 *   - registration FSM (IDLE -> ATTACH_PENDING -> REGISTERED -> ...)
 *   - msgbus glue: TleSap from LLC carries decoded LLC PDUs with MLE-PD
 *     prefix. MLE inspects MLE-disc (3 bits):
 *       * disc=001 (=1) -> MM PDU body, dispatched to MM via TlmcSap
 *       * disc=101 (=5) -> MLE-itself (D-NWRK-BCAST etc.)
 *   - subscriber-DB lookups (Entity / AST / Profile) via S5 IF_DB_API_v1
 *
 * Cross-agent assumption: MLE-disc=1 (MM) routes through TlmcSap to MM
 * (MLE management plane); MLE-disc=5 (MLE-itself) is consumed inside MLE.
 * MLE never directly invokes MM functions outside the bus.
 */
#ifndef TETRA_MLE_H
#define TETRA_MLE_H

#include "tetra/db.h"
#include "tetra/llc.h"
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * MLE-PD (Protocol-Discriminator) — 3-bit field at the start of every
 * LLC body. Per ETSI Tab 18.4.
 * ------------------------------------------------------------------------- */
typedef enum {
    MleDisc_Reserved0   = 0,
    MleDisc_Mm          = 1,
    MleDisc_Cmce        = 2,
    MleDisc_Reserved3   = 3,
    MleDisc_Sndcp       = 4,
    MleDisc_MleItself   = 5,
    MleDisc_TmcoSync    = 6,
    MleDisc_Reserved7   = 7
} MleDisc;

/* ---------------------------------------------------------------------------
 * MleState — FSM states per docs/MIGRATION_PLAN.md §S3.
 * ------------------------------------------------------------------------- */
typedef enum {
    MleState_Idle           = 0,
    MleState_AttachPending  = 1,
    MleState_Registered     = 2,
    MleState_GroupAttach    = 3,
    MleState_Detaching      = 4
} MleState;

/* ---------------------------------------------------------------------------
 * MleEvt — events that drive the FSM.
 * ------------------------------------------------------------------------- */
typedef enum {
    MleEvt_DemandReceived       = 0,
    MleEvt_AcceptSent           = 1,
    MleEvt_AcceptAcked          = 2,  /* MS BL-ACK on D-LOC-UPDATE-ACCEPT */
    MleEvt_GrpDemandReceived    = 3,
    MleEvt_GrpAckSent           = 4,
    MleEvt_DetachReceived       = 5,
    MleEvt_DetachComplete       = 6
} MleEvt;

/* ---------------------------------------------------------------------------
 * MleSession — per-MS registration session.
 *
 * Indexed by ISSI; max MLE_SESSION_MAX active sessions. The session caches
 * the AST shadow_idx so MM can read/write the AST slot on attach/detach
 * without a second linear scan.
 * ------------------------------------------------------------------------- */
#define MLE_SESSION_MAX 16u

typedef struct {
    bool        in_use;
    uint32_t    issi;            /* low 24 bits */
    EndpointId  endpoint;
    MleState    state;
    uint8_t     gila_class;      /* from Profile (M2 default = 4) */
    uint8_t     gila_lifetime;   /* from Profile (M2 default = 1) */
    uint8_t     pending_giu_count;  /* group identities in flight */
    /* Gold-Ref M2 default GSSI for accept fallback (when MS demanded none) */
    uint32_t    fallback_gssi;
} MleSession;

/* ---------------------------------------------------------------------------
 * MleCfg — knobs.
 *
 * accept_unknown : if true, BS auto-enrolls unknown ISSIs in Profile 0.
 *                  Mirrors REG_DB_POLICY[0] (reference_subscriber_db_arch.md).
 * default_profile_id : profile ID for auto-enroll (default = 0).
 * fallback_gssi  : GSSI to attach when MS demanded none (Gold-Ref:
 *                  0x2F4D61 for cell GSSI).
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     accept_unknown;
    uint8_t  default_profile_id;
    uint32_t fallback_gssi;
} MleCfg;

typedef struct {
    size_t demands_received;
    size_t accepts_sent;
    size_t grp_demands_received;
    size_t grp_acks_sent;
    size_t lookups_failed;
    size_t fsm_transitions;
} MleStats;

typedef struct {
    MsgBus       *bus;
    SubscriberDb *db;
    MleCfg        cfg;
    MleSession    sessions[MLE_SESSION_MAX];
    MleStats      stats;
    bool          initialised;
} Mle;

/* ---------------------------------------------------------------------------
 * TleSap message arrives on (MLE, TleSap) — payload is a TleSapMsg struct.
 * MLE inspects pdu.body[0..2] = MLE-PD then dispatches.
 *
 * TlaSap (MLE -> User / management) carries decoded MM-decoded events upward
 * to MM-internal logic via msgbus. We re-use the SapMsg envelope; payload is
 * a TlaSapMsg struct (mle->mm/cmce/...).
 * ------------------------------------------------------------------------- */
typedef struct {
    EndpointId   endpoint;
    TetraAddress addr;
    MleDisc      disc;            /* MLE-PD */
    uint16_t     body_len_bits;   /* MM/CMCE/... body bits, post-MLE-PD strip */
    uint8_t      body[LLC_PDU_BODY_MAX_BYTES];
} TleSapPayload;

/* TlaSapPayload is defined in mm.h (included above) — MLE <-> MM envelope. */

/* ---------------------------------------------------------------------------
 * IF_MLE_v1 — public API.
 *
 * mle_init  : zero-init the entity, register handlers on the bus.
 * mle_handle_tle_msg : entry from LLC->MLE (decoded LLC PDU including
 *                      MLE-PD prefix). Steers by MLE-disc.
 * mle_handle_tla_msg : entry from MM->MLE (e.g. accept finalised).
 * ------------------------------------------------------------------------- */

int mle_init(Mle *mle, MsgBus *bus, SubscriberDb *db, const MleCfg *cfg);

int mle_handle_tle_msg(Mle *mle, const TleSapMsg *msg);

int mle_handle_tla_msg(Mle *mle, const TlaSapPayload *msg);

/* ---------------------------------------------------------------------------
 * FSM helpers — exposed for tests.
 * ------------------------------------------------------------------------- */

MleSession *mle_session_lookup(Mle *mle, uint32_t issi);
MleSession *mle_session_alloc(Mle *mle, uint32_t issi);

/* mle_fsm_step  : drive the per-session FSM with a new event. Updates
 * `state` in place and returns the new state. */
MleState mle_fsm_step(MleSession *s, MleEvt e);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_MLE_H */

/* sw/mle/mle.c — MLE entity + msgbus glue.
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MLE_v1.
 *
 * Registration-side responsibilities:
 *   1. Receive TleSapMsg from LLC (MLE-PD prefix + upper-layer body)
 *   2. Inspect MLE-PD (3 bits at body[0..2]) to route:
 *        disc=001 (MM)         -> decode + enqueue MM-request via TlmcSap
 *        disc=101 (MLE-itself) -> consume locally (D-NWRK-BCAST etc.)
 *        else                  -> drop (unsupported)
 *   3. Drive per-MS FSM via mle_fsm_step() on demand/accept events
 *   4. Lookup Subscriber-DB Entity → Profile to feed gila_class /
 *      gila_lifetime defaults to MM
 *   5. Receive accept-completed feedback on TlaSap from MM and step the
 *      FSM forward (AttachPending → Registered, etc.)
 *
 * Routing on the bus (S3 cross-agent decision): MM listens on
 * (TmaSap, TlmcSap); MLE listens on (TmaSap, TleSap). The (TmaSap, ...)
 * dest field is reused as a "consumer slot" — bluestation has separate
 * SAPs but our minimal SapId enum collapses similar consumers under
 * SapId_TmaSap + a SAP discriminator. This keeps MSGBUS_REG_CAP small
 * while preserving disc-level routing.
 */
#include "tetra/db.h"
#include "tetra/llc.h"
#include "tetra/mle.h"
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Session lookup / alloc.
 * ------------------------------------------------------------------------- */
MleSession *mle_session_lookup(Mle *mle, uint32_t issi)
{
    if (mle == NULL) {
        return NULL;
    }
    issi &= TETRA_SSI_MASK_24;
    for (size_t i = 0; i < MLE_SESSION_MAX; ++i) {
        if (mle->sessions[i].in_use && mle->sessions[i].issi == issi) {
            return &mle->sessions[i];
        }
    }
    return NULL;
}

MleSession *mle_session_alloc(Mle *mle, uint32_t issi)
{
    if (mle == NULL) {
        return NULL;
    }
    issi &= TETRA_SSI_MASK_24;
    MleSession *existing = mle_session_lookup(mle, issi);
    if (existing != NULL) {
        return existing;
    }
    for (size_t i = 0; i < MLE_SESSION_MAX; ++i) {
        if (!mle->sessions[i].in_use) {
            MleSession *s = &mle->sessions[i];
            memset(s, 0, sizeof(*s));
            s->in_use        = true;
            s->issi          = issi;
            s->state         = MleState_Idle;
            s->gila_class    = 4;  /* Profile-0 default */
            s->gila_lifetime = 1;
            s->fallback_gssi = mle->cfg.fallback_gssi;
            return s;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * DB-side helpers.
 * ------------------------------------------------------------------------- */
static bool refresh_session_from_db(Mle *mle, MleSession *s)
{
    if (mle == NULL || mle->db == NULL || s == NULL) {
        return false;
    }
    uint16_t idx = 0;
    if (db_lookup_entity(mle->db, s->issi, &idx) != 0) {
        return false;
    }
    Entity e;
    if (db_get_entity(mle->db, idx, &e) != 0 || !e.valid) {
        return false;
    }
    Profile pr;
    if (db_get_profile(mle->db, e.profile_id, &pr) != 0 || !pr.valid) {
        return false;
    }
    s->gila_class    = pr.gila_class;
    s->gila_lifetime = pr.gila_lifetime;
    return true;
}

/* Auto-enroll an unknown ISSI under default_profile_id if accept_unknown.
 * Returns true if enrolled (or already known), false if rejected. */
static bool maybe_auto_enroll(Mle *mle, uint32_t issi)
{
    if (mle == NULL || mle->db == NULL) {
        return mle->cfg.accept_unknown;
    }
    uint16_t idx = 0;
    if (db_lookup_entity(mle->db, issi & TETRA_SSI_MASK_24, &idx) == 0) {
        return true;
    }
    if (!mle->cfg.accept_unknown) {
        mle->stats.lookups_failed++;
        return false;
    }
    /* Find a free slot. */
    for (uint16_t i = 0; i < TETRA_DB_ENTITY_COUNT; ++i) {
        Entity e;
        if (db_get_entity(mle->db, i, &e) != 0) {
            continue;
        }
        if (!e.valid) {
            memset(&e, 0, sizeof(e));
            e.entity_id   = issi & TETRA_SSI_MASK_24;
            e.entity_type = 0;  /* ISSI */
            e.profile_id  = mle->cfg.default_profile_id;
            e.valid       = true;
            (void) db_put_entity(mle->db, i, &e);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Decode MLE-PD from LLC body (first 3 bits) and return the rest.
 * Returns 0 on success and writes the post-MLE-PD body into *out_body.
 * ------------------------------------------------------------------------- */
static int strip_mle_pd(const LlcPdu *pdu, MleDisc *out_disc,
                        uint8_t *out_body, size_t out_cap_bits,
                        size_t *out_body_len_bits)
{
    if (pdu == NULL || out_disc == NULL || out_body == NULL ||
        out_body_len_bits == NULL) {
        return -EINVAL;
    }
    if (pdu->body_len_bits < 3u) {
        return -EPROTO;
    }
    /* Read the 3-bit MLE-PD MSB-first from body[0]. */
    const uint8_t b0 = pdu->body[0];
    *out_disc = (MleDisc) ((b0 >> 5) & 0x7u);

    const size_t rest_bits = (size_t) pdu->body_len_bits - 3u;
    if (rest_bits > out_cap_bits) {
        return -ENOSPC;
    }

    /* Shift the entire body left by 3 bits, MSB-first, into out_body. */
    memset(out_body, 0, (out_cap_bits + 7u) / 8u);
    for (size_t i = 0; i < rest_bits; ++i) {
        const size_t  src_bit  = 3u + i;
        const size_t  src_byte = src_bit >> 3;
        const uint8_t src_off  = (uint8_t) (7u - (src_bit & 0x7u));
        const uint8_t bit      = (uint8_t) ((pdu->body[src_byte] >> src_off) & 0x1u);
        if (bit) {
            const size_t  dst_byte = i >> 3;
            const uint8_t dst_off  = (uint8_t) (7u - (i & 0x7u));
            out_body[dst_byte] |= (uint8_t) (1u << dst_off);
        }
    }
    *out_body_len_bits = rest_bits;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Forward a decoded MM PDU as a TlaSapPayload via msgbus on TlmcSap.
 * MM's on_mm_request handler picks it up.
 * ------------------------------------------------------------------------- */
static int forward_mm_request(Mle *mle, EndpointId endpoint,
                              const TetraAddress *addr,
                              const MmDecoded *decoded)
{
    if (mle == NULL || addr == NULL || decoded == NULL) {
        return -EINVAL;
    }
    TlaSapPayload pl;
    memset(&pl, 0, sizeof(pl));
    pl.endpoint   = endpoint;
    pl.addr       = *addr;
    pl.decoded    = *decoded;
    pl.is_attach  = (decoded->location_update_type == LocUpdate_ItsiAttach) ||
                    (decoded->pdu_type == MmPduUl_UAttachDetachGroupIdentity);

    SapMsg env;
    sapmsg_init(&env, SapId_TleSap, SapId_TmaSap, SapId_TlmcSap,
                (const uint8_t *) &pl, (uint16_t) sizeof(pl));
    return msgbus_post(mle->bus, sap_prio_default(SapId_TlmcSap), &env);
}

/* ---------------------------------------------------------------------------
 * Handle a MM-class TleSapMsg (MLE-disc=001).
 * Decodes the MM body, drives the FSM, optionally enrolls the MS, and
 * forwards a request to MM.
 * ------------------------------------------------------------------------- */
static int handle_mm_pdu(Mle *mle, const TleSapMsg *m,
                         const uint8_t *body, size_t body_bits)
{
    MmDecoded decoded;
    int rc = mm_iep_decode(body, body_bits, &decoded);
    if (rc != 0) {
        return rc;
    }

    MleSession *s = mle_session_alloc(mle, m->addr.ssi);
    if (s == NULL) {
        return -ENOSPC;
    }
    s->endpoint = m->endpoint;

    /* DB lookup / auto-enroll. */
    (void) maybe_auto_enroll(mle, m->addr.ssi);
    (void) refresh_session_from_db(mle, s);

    /* Drive the FSM. */
    switch (decoded.pdu_type) {
    case MmPduUl_ULocationUpdateDemand:
        mle->stats.demands_received++;
        (void) mle_fsm_step(s, MleEvt_DemandReceived);
        mle->stats.fsm_transitions++;
        break;
    case MmPduUl_UAttachDetachGroupIdentity:
        mle->stats.grp_demands_received++;
        (void) mle_fsm_step(s, MleEvt_GrpDemandReceived);
        mle->stats.fsm_transitions++;
        break;
    case MmPduUl_UItsiDetach:
        (void) mle_fsm_step(s, MleEvt_DetachReceived);
        mle->stats.fsm_transitions++;
        break;
    default:
        /* Unsupported MM PDU type — keep state, drop silently. */
        return 0;
    }

    /* Forward to MM for accept-build. */
    return forward_mm_request(mle, m->endpoint, &m->addr, &decoded);
}

/* ---------------------------------------------------------------------------
 * Handle MLE-itself PDU (disc=5): D-NWRK-BCAST etc. Outside this slice's
 * scope (S4 owns CMCE + D-NWRK-BCAST). MLE simply records receipt and
 * drops.
 * ------------------------------------------------------------------------- */
static int handle_mle_itself(Mle *mle, const TleSapMsg *m,
                             const uint8_t *body, size_t body_bits)
{
    (void) mle;
    (void) m;
    (void) body;
    (void) body_bits;
    /* Future: parse mle_prim @ body[0..2], dispatch to NWRK / D-NEW-CELL etc. */
    return 0;
}

/* ---------------------------------------------------------------------------
 * mle_handle_tle_msg — entry from LLC (TleSap).
 * ------------------------------------------------------------------------- */
int mle_handle_tle_msg(Mle *mle, const TleSapMsg *m)
{
    if (mle == NULL || m == NULL || !mle->initialised) {
        return -EINVAL;
    }

    MleDisc disc;
    uint8_t after_pd[LLC_PDU_BODY_MAX_BYTES];
    size_t  after_pd_bits = 0;
    int rc = strip_mle_pd(&m->pdu, &disc, after_pd,
                          sizeof(after_pd) * 8u, &after_pd_bits);
    if (rc != 0) {
        return rc;
    }

    switch (disc) {
    case MleDisc_Mm:
        return handle_mm_pdu(mle, m, after_pd, after_pd_bits);
    case MleDisc_MleItself:
        return handle_mle_itself(mle, m, after_pd, after_pd_bits);
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * mle_handle_tla_msg — entry from MM (TlaSap).
 *
 * MM signals "accept built and posted to LLC". MLE advances the FSM.
 * The payload distinguishes attach vs group-attach via decoded.pdu_type.
 * ------------------------------------------------------------------------- */
int mle_handle_tla_msg(Mle *mle, const TlaSapPayload *m)
{
    if (mle == NULL || m == NULL || !mle->initialised) {
        return -EINVAL;
    }
    MleSession *s = mle_session_lookup(mle, m->addr.ssi);
    if (s == NULL) {
        return -ENOENT;
    }

    switch (m->decoded.pdu_type) {
    case MmPduUl_ULocationUpdateDemand:
        (void) mle_fsm_step(s, MleEvt_AcceptSent);
        mle->stats.accepts_sent++;
        mle->stats.fsm_transitions++;
        break;
    case MmPduUl_UAttachDetachGroupIdentity:
        (void) mle_fsm_step(s, MleEvt_GrpAckSent);
        mle->stats.grp_acks_sent++;
        mle->stats.fsm_transitions++;
        break;
    case MmPduUl_UItsiDetach:
        (void) mle_fsm_step(s, MleEvt_DetachComplete);
        mle->stats.fsm_transitions++;
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * msgbus handlers.
 * ------------------------------------------------------------------------- */
static void on_tle_msg(const SapMsg *msg, void *ctx)
{
    Mle *mle = (Mle *) ctx;
    if (mle == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TleSapMsg)) {
        return;
    }
    TleSapMsg in;
    memcpy(&in, msg->payload, sizeof(in));
    (void) mle_handle_tle_msg(mle, &in);
}

static void on_tla_msg(const SapMsg *msg, void *ctx)
{
    Mle *mle = (Mle *) ctx;
    if (mle == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TlaSapPayload)) {
        return;
    }
    TlaSapPayload pl;
    memcpy(&pl, msg->payload, sizeof(pl));
    (void) mle_handle_tla_msg(mle, &pl);
}

/* ---------------------------------------------------------------------------
 * mle_init.
 * ------------------------------------------------------------------------- */
int mle_init(Mle *mle, MsgBus *bus, SubscriberDb *db, const MleCfg *cfg)
{
    if (mle == NULL || bus == NULL) {
        return -EINVAL;
    }
    memset(mle, 0, sizeof(*mle));
    mle->bus = bus;
    mle->db  = db;
    if (cfg != NULL) {
        mle->cfg = *cfg;
    }

    /* MLE listens for upward TleSap messages from LLC. The LLC posts
     * with dest=TmaSap (its "consumer" slot) and sap=TleSap. */
    int rc = msgbus_register(bus, SapId_TmaSap, SapId_TleSap,
                             on_tle_msg, mle);
    if (rc != 0) {
        return rc;
    }

    /* MLE also listens for accept-completed feedback from MM (TlaSap).
     * MM posts with dest=TmaSap, sap=TnmmSap (re-using TnmmSap as the
     * MLE-feedback channel — bluestation has TlmcSap for this). */
    rc = msgbus_register(bus, SapId_TmaSap, SapId_TnmmSap,
                         on_tla_msg, mle);
    if (rc != 0) {
        return rc;
    }

    mle->initialised = true;
    return 0;
}

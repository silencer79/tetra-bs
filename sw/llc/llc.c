/* sw/llc/llc.c — LLC main entity (state machine + retransmission).
 *
 * Owned by S2 (S2-sw-llc). Locked under interface contract IF_LLC_v1.
 *
 * Logical Link Control per ETSI EN 300 392-2 §22 + bluestation `llc/`.
 *   - per-endpoint NR/NS state (stop-and-wait, modulo-2)
 *   - retransmission of unacked BL-(A)DATA up to LlcCfg.max_retx
 *   - msgbus glue: receives TmaUnitdataInd via the (LLC, TmaSap) tuple,
 *     posts TleSapMsg upward to MLE; receives TleSapMsg from MLE on the
 *     (LLC, TleSap) tuple and emits TmaUnitdataReq downward.
 *
 * Memory: zero-malloc, zero-thread. Endpoint slots are a fixed array
 * sized at LLC_MAX_ENDPOINTS. Storage for msgbus payload is the bus's
 * own (S0 owns it).
 */
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

static LlcEndpoint *endpoint_find(Llc *llc, EndpointId id)
{
    for (size_t i = 0; i < LLC_MAX_ENDPOINTS; ++i) {
        if (llc->endpoints[i].in_use && llc->endpoints[i].id == id) {
            return &llc->endpoints[i];
        }
    }
    return NULL;
}

static LlcEndpoint *endpoint_alloc(Llc *llc, EndpointId id)
{
    for (size_t i = 0; i < LLC_MAX_ENDPOINTS; ++i) {
        if (!llc->endpoints[i].in_use) {
            LlcEndpoint *ep = &llc->endpoints[i];
            memset(ep, 0, sizeof(*ep));
            ep->id          = id;
            ep->in_use      = true;
            ep->ns_send     = 0;
            ep->nr_expected = 0;
            ep->awaiting_ack = false;
            ep->retx_count   = 0;
            return ep;
        }
    }
    return NULL;
}

LlcEndpoint *llc_endpoint_lookup(Llc *llc, EndpointId id)
{
    if (llc == NULL) {
        return NULL;
    }
    LlcEndpoint *ep = endpoint_find(llc, id);
    if (ep != NULL) {
        return ep;
    }
    return endpoint_alloc(llc, id);
}

/* Encode an LlcPdu into a stack buffer; output the bit-length and a
 * pointer into the same buffer. Returns 0 on success, negative on error. */
static int encode_to_bytes(const LlcPdu *pdu,
                           uint8_t *buf, size_t buf_cap_bytes,
                           uint16_t *out_len_bits)
{
    BitBuffer bb = bb_init_autoexpand(buf, buf_cap_bytes * 8u);
    const int n  = llc_pdu_encode(&bb, pdu);
    if (n < 0) {
        return n;
    }
    *out_len_bits = (uint16_t) n;
    return 0;
}

/* ---------------------------------------------------------------------------
 * msgbus handler — TleSap downstream (MLE → LLC).
 *
 * MLE posts a TleSapMsg with `pdu` already filled (pdu_type, body, etc.).
 * For BL-DATA / BL-ADATA we stamp NS from our endpoint state, store the
 * frame for retransmit, encode it, and emit a TmaUnitdataReq downward.
 * For BL-ACK we stamp NR similarly. AL-SETUP is forwarded verbatim
 * (wrapper, no state).
 * ------------------------------------------------------------------------- */
static void on_tle_msg_from_mle(const SapMsg *msg, void *ctx)
{
    Llc *llc = (Llc *) ctx;
    if (llc == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TleSapMsg)) {
        return;
    }
    if (!llc->initialised) {
        return;
    }

    /* Copy out of the queue payload — we'll dispatch synchronously. */
    TleSapMsg in;
    memcpy(&in, msg->payload, sizeof(in));

    LlcEndpoint *ep = llc_endpoint_lookup(llc, in.endpoint);
    if (ep == NULL) {
        return;
    }

    /* Stamp NR/NS from endpoint state. */
    LlcPdu pdu = in.pdu;
    if (llc_pdu_type_has_ns(pdu.pdu_type)) {
        pdu.ns = ep->ns_send;
    }
    if (llc_pdu_type_has_nr(pdu.pdu_type)) {
        pdu.nr = ep->nr_expected;
    }

    /* Encode + emit downward. */
    uint8_t  bytes[LLC_PDU_BODY_MAX_BYTES + 8u];
    uint16_t bits = 0;
    if (encode_to_bytes(&pdu, bytes, sizeof(bytes), &bits) != 0) {
        return;
    }

    TmaUnitdataReq req = {0};
    req.endpoint     = in.endpoint;
    req.addr         = in.addr;
    req.sdu_len_bits = bits;
    if (bits > TMA_SDU_MAX_BYTES * 8u) {
        return;
    }
    memcpy(req.sdu_bits, bytes, (bits + 7u) / 8u);

    SapMsg out;
    sapmsg_init(&out, SapId_TleSap, SapId_TmaSap, SapId_TmaSap,
                (const uint8_t *) &req, (uint16_t) sizeof(req));
    if (msgbus_post(llc->bus, sap_prio_default(SapId_TmaSap), &out) == 0) {
        llc->stats.pdus_tx++;
    }

    /* For BL-(A)DATA, stash for retransmit + flip "awaiting ack". */
    if (pdu.pdu_type == LlcPdu_BL_DATA  || pdu.pdu_type == LlcPdu_BL_DATA_FCS  ||
        pdu.pdu_type == LlcPdu_BL_ADATA || pdu.pdu_type == LlcPdu_BL_ADATA_FCS) {
        ep->last_sent     = pdu;
        ep->awaiting_ack  = true;
        ep->retx_count    = 0;
        ep->ns_send       = (uint8_t) (ep->ns_send ^ 0x1u);
    }
    /* BL-ACK / BL-UDATA / AL-SETUP are "send and forget" from LLC POV. */
}

/* ---------------------------------------------------------------------------
 * msgbus handler — TmaSap RX (UMAC → LLC).
 *
 * Bus-level wrapper: unpacks the TmaUnitdataInd payload from the SapMsg
 * and delegates to llc_handle_tma_unitdata_ind.
 * ------------------------------------------------------------------------- */
static void on_tma_unitdata_ind(const SapMsg *msg, void *ctx)
{
    Llc *llc = (Llc *) ctx;
    if (llc == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TmaUnitdataInd)) {
        return;
    }
    TmaUnitdataInd ind;
    memcpy(&ind, msg->payload, sizeof(ind));
    (void) llc_handle_tma_unitdata_ind(llc, &ind);
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

int llc_init(Llc *llc, MsgBus *bus, const LlcCfg *cfg)
{
    if (llc == NULL || bus == NULL) {
        return -EINVAL;
    }

    memset(llc, 0, sizeof(*llc));
    llc->bus = bus;
    if (cfg != NULL) {
        llc->cfg = *cfg;
    }
    if (llc->cfg.max_retx == 0u) {
        llc->cfg.max_retx = LLC_DEFAULT_MAX_RETX;
    }

    /* Register on TmaSap RX (MAC → LLC). */
    int rc = msgbus_register(bus, SapId_TmaSap, SapId_TmaSap,
                             on_tma_unitdata_ind, llc);
    if (rc != 0) {
        return rc;
    }
    /* Register on TleSap (MLE → LLC) — same callback ctx. */
    rc = msgbus_register(bus, SapId_TmaSap, SapId_TleSap,
                         on_tle_msg_from_mle, llc);
    if (rc != 0) {
        return rc;
    }

    llc->initialised = true;
    return 0;
}

int llc_post_to_mle(Llc *llc, const TleSapMsg *msg)
{
    if (llc == NULL || msg == NULL) {
        return -EINVAL;
    }

    SapMsg env;
    sapmsg_init(&env, SapId_TmaSap, SapId_TleSap, SapId_TleSap,
                (const uint8_t *) msg, (uint16_t) sizeof(*msg));
    return msgbus_post(llc->bus, sap_prio_default(SapId_TleSap), &env);
}

/* ---------------------------------------------------------------------------
 * llc_handle_tma_unitdata_ind — parse the SDU, drive the per-endpoint
 * state machine, optionally re-ACK or forward upward.
 *
 * State transitions per ETSI §22 (stop-and-wait, modulo-2):
 *   Receive BL-DATA / BL-ADATA with NS == nr_expected:
 *     → forward to MLE, advance nr_expected, send BL-ACK with new NR.
 *   Receive BL-DATA / BL-ADATA with NS != nr_expected:
 *     → discard (do not forward), re-send BL-ACK with old NR (peer
 *       missed the previous ACK).
 *   Receive BL-ACK with NR == ns_send (= the NS we just sent + 1 mod 2):
 *     → clear awaiting_ack, our last frame was accepted.
 *   Receive BL-ACK with NR != that:
 *     → ignore (peer is acking an older frame; we will retransmit on
 *       our timer if awaiting_ack is still set).
 *   Receive BL-UDATA / AL-SETUP / unknown:
 *     → forward to MLE (no state change).
 * ------------------------------------------------------------------------- */
int llc_handle_tma_unitdata_ind(Llc *llc, const TmaUnitdataInd *ind)
{
    if (llc == NULL || ind == NULL) {
        return -EINVAL;
    }
    if (ind->sdu_len_bits == 0u || ind->sdu_len_bits > TMA_SDU_MAX_BYTES * 8u) {
        return -EINVAL;
    }

    /* Parse the LLC PDU. The MAC has already stripped its header; the
     * SDU bits start at the LLC pdu_type field. We need to know the
     * body length so we can read the right number of bits — derive it
     * from the SDU length minus the LLC overhead per type. */

    /* First peek pdu_type without consuming bb's cursor — we need it
     * to compute the body length. */
    uint8_t   sdu_buf[TMA_SDU_MAX_BYTES];
    memcpy(sdu_buf, ind->sdu_bits, (ind->sdu_len_bits + 7u) / 8u);
    BitBuffer bb = bb_init(sdu_buf, ind->sdu_len_bits);

    if (bb_remaining(&bb) < 4u) {
        return -EPROTO;
    }
    const uint32_t raw_type = bb_get_bits(&bb, 4);
    if (!llc_pdu_type_is_valid((uint8_t) raw_type)) {
        llc->stats.unknown_pdu_type++;
        return -EPROTO;
    }
    const LlcPduType type = (LlcPduType) raw_type;

    /* Compute overhead bits (pdu_type + NR + NS + FCS). */
    size_t overhead = 4u; /* pdu_type */
    if (llc_pdu_type_has_nr(type)) overhead += 1u;
    if (llc_pdu_type_has_ns(type)) overhead += 1u;
    if (llc_pdu_type_has_fcs(type)) overhead += 32u;

    if (type == LlcPdu_AL_SETUP) {
        /* Wrapper only — no body. */
        overhead = 4u;
    }

    if ((size_t) ind->sdu_len_bits < overhead) {
        return -EPROTO;
    }
    const uint16_t body_bits = (type == LlcPdu_AL_SETUP)
        ? (uint16_t) 0u
        : (uint16_t) ((size_t) ind->sdu_len_bits - overhead);
    if (body_bits > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return -EPROTO;
    }

    /* Reset cursor and decode for real. */
    bb_seek_bits(&bb, 0);
    LlcPdu pdu;
    memset(&pdu, 0, sizeof(pdu));
    pdu.body_len_bits = body_bits;
    const int rc = llc_pdu_decode(&bb, &pdu);
    if (rc != 0) {
        return rc;
    }

    llc->stats.pdus_rx++;

    if (llc_pdu_type_has_fcs(pdu.pdu_type) && !pdu.fcs_valid) {
        llc->stats.fcs_failures++;
        /* ETSI §22.4.4: discard frames with FCS errors silently. */
        return 0;
    }

    LlcEndpoint *ep = llc_endpoint_lookup(llc, ind->endpoint);
    if (ep == NULL) {
        return -ENOSPC;
    }

    switch (pdu.pdu_type) {
    case LlcPdu_BL_DATA: case LlcPdu_BL_DATA_FCS:
    case LlcPdu_BL_ADATA: case LlcPdu_BL_ADATA_FCS: {
        /* Stop-and-wait RX side. */
        if (pdu.ns == ep->nr_expected) {
            /* In-sequence — forward upward + flip nr_expected + send ACK. */
            ep->nr_expected = (uint8_t) (ep->nr_expected ^ 0x1u);
            TleSapMsg up = {0};
            up.endpoint = ind->endpoint;
            up.addr     = ind->addr;
            up.pdu      = pdu;
            (void) llc_post_to_mle(llc, &up);
            (void) llc_send_bl_ack(llc, ind->endpoint, &ind->addr);
        } else {
            /* Out-of-sequence retransmit — re-ACK previous NR. */
            (void) llc_send_bl_ack(llc, ind->endpoint, &ind->addr);
        }
        break;
    }
    case LlcPdu_BL_ACK: case LlcPdu_BL_ACK_FCS: {
        /* If NR == ns_send (= the next NS we'd send), the peer has
         * accepted our previous frame. */
        if (ep->awaiting_ack && pdu.nr == ep->ns_send) {
            ep->awaiting_ack = false;
            ep->retx_count   = 0;
        }
        /* BL-ACK never propagates upward — it's an LLC-internal control. */
        break;
    }
    case LlcPdu_BL_UDATA: case LlcPdu_BL_UDATA_FCS:
    case LlcPdu_AL_SETUP:
    default: {
        /* Stateless — forward upward. */
        TleSapMsg up = {0};
        up.endpoint = ind->endpoint;
        up.addr     = ind->addr;
        up.pdu      = pdu;
        (void) llc_post_to_mle(llc, &up);
        break;
    }
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * llc_send_bl_data — TX side of stop-and-wait.
 * ------------------------------------------------------------------------- */
int llc_send_bl_data(Llc *llc, EndpointId endpoint, const TetraAddress *addr,
                     const uint8_t *body, uint16_t body_len_bits)
{
    if (llc == NULL || addr == NULL ||
        (body == NULL && body_len_bits != 0u) ||
        body_len_bits > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return -EINVAL;
    }
    LlcEndpoint *ep = llc_endpoint_lookup(llc, endpoint);
    if (ep == NULL) {
        return -ENOSPC;
    }
    if (ep->awaiting_ack) {
        return -EBUSY;
    }

    LlcPdu pdu = {0};
    pdu.pdu_type      = LlcPdu_BL_DATA;
    pdu.ns            = ep->ns_send;
    pdu.body_len_bits = body_len_bits;
    if (body_len_bits > 0u) {
        memcpy(pdu.body, body, (body_len_bits + 7u) / 8u);
    }

    uint8_t  bytes[LLC_PDU_BODY_MAX_BYTES + 8u];
    uint16_t bits = 0;
    int rc = encode_to_bytes(&pdu, bytes, sizeof(bytes), &bits);
    if (rc != 0) {
        return rc;
    }

    TmaUnitdataReq req = {0};
    req.endpoint     = endpoint;
    req.addr         = *addr;
    req.sdu_len_bits = bits;
    if (bits > TMA_SDU_MAX_BYTES * 8u) {
        return -E2BIG;
    }
    memcpy(req.sdu_bits, bytes, (bits + 7u) / 8u);

    SapMsg env;
    sapmsg_init(&env, SapId_TleSap, SapId_TmaSap, SapId_TmaSap,
                (const uint8_t *) &req, (uint16_t) sizeof(req));
    rc = msgbus_post(llc->bus, sap_prio_default(SapId_TmaSap), &env);
    if (rc != 0) {
        return rc;
    }

    ep->last_sent    = pdu;
    ep->awaiting_ack = true;
    ep->retx_count   = 0;
    ep->ns_send      = (uint8_t) (ep->ns_send ^ 0x1u);
    llc->stats.pdus_tx++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * llc_send_bl_ack — emit a BL-ACK with the current nr_expected.
 * ------------------------------------------------------------------------- */
int llc_send_bl_ack(Llc *llc, EndpointId endpoint, const TetraAddress *addr)
{
    if (llc == NULL || addr == NULL) {
        return -EINVAL;
    }
    LlcEndpoint *ep = llc_endpoint_lookup(llc, endpoint);
    if (ep == NULL) {
        return -ENOSPC;
    }

    LlcPdu pdu = {0};
    pdu.pdu_type      = LlcPdu_BL_ACK;
    pdu.nr            = ep->nr_expected;
    pdu.body_len_bits = 0;

    uint8_t  bytes[8u];
    uint16_t bits = 0;
    int rc = encode_to_bytes(&pdu, bytes, sizeof(bytes), &bits);
    if (rc != 0) {
        return rc;
    }

    TmaUnitdataReq req = {0};
    req.endpoint     = endpoint;
    req.addr         = *addr;
    req.sdu_len_bits = bits;
    memcpy(req.sdu_bits, bytes, (bits + 7u) / 8u);

    SapMsg env;
    sapmsg_init(&env, SapId_TleSap, SapId_TmaSap, SapId_TmaSap,
                (const uint8_t *) &req, (uint16_t) sizeof(req));
    rc = msgbus_post(llc->bus, sap_prio_default(SapId_TmaSap), &env);
    if (rc != 0) {
        return rc;
    }

    llc->stats.pdus_tx++;
    return 0;
}

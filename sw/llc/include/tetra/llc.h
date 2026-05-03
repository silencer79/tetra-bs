/* sw/llc/include/tetra/llc.h — LLC public API.
 *
 * Owned by S2 (S2-sw-llc). Locked under interface contract IF_LLC_v1.
 *
 * Logical Link Control per ETSI EN 300 392-2 §22 + bluestation `llc/`.
 * PDU encoder/decoder (BL-DATA, BL-ADATA, BL-UDATA, BL-ACK, AL-SETUP),
 * NR/NS sequence-tracking per endpoint, FCS (CRC-32, ETSI polynomial),
 * stop-and-wait retransmission state-machine.
 *
 * Source-of-truth hierarchy (CLAUDE.md §1): Gold > Bluestation > ETSI.
 *   - Gold-Ref: docs/references/reference_gold_attach_bitexact.md
 *     UL#0 BL-DATA (NS only, no FCS), UL#2 BL-ACK (NR only, no FCS),
 *     DL#735 BL-ADATA (NR+NS, no FCS in this capture), DL#727 AL-SETUP
 *     (wrapper, no MM body).
 *   - LLC PDU type table per scripts/decode_ul_raw.py:LLC_PDU_TYPE
 *     mirrors ETSI EN 300 392-2 Table 22.1 (4-bit pdu_type field).
 *
 * Pure header — no .c. Includes tetra/types.h + tetra/msgbus.h from S0.
 */
#ifndef TETRA_LLC_H
#define TETRA_LLC_H

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
 * LLC PDU type — 4-bit field per ETSI EN 300 392-2 Tab 22.1, mirrored from
 * scripts/decode_ul_raw.py:LLC_PDU_TYPE which itself mirrors bluestation
 * `llc/pdu_type.rs`. Values are bit-exact on-air.
 *
 * BL-class = Basic Link (stop-and-wait, single-fragment).
 * AL-class = Advanced Link (DL#727 pre-reply wrapper observed).
 *
 * The "+FCS" variants (0x4..0x7) are the same PDU but with a 32-bit FCS
 * trailing the body. Encoder is parameterised by a `with_fcs` boolean
 * which selects the +4 offset.
 * ------------------------------------------------------------------------- */
typedef enum {
    LlcPdu_BL_ADATA      = 0x0,  /* NR+NS, no FCS                            */
    LlcPdu_BL_DATA       = 0x1,  /* NS only, no FCS                          */
    LlcPdu_BL_UDATA      = 0x2,  /* broadcast, no NR/NS, no FCS              */
    LlcPdu_BL_ACK        = 0x3,  /* NR only, no FCS                          */
    LlcPdu_BL_ADATA_FCS  = 0x4,  /* NR+NS + FCS                              */
    LlcPdu_BL_DATA_FCS   = 0x5,  /* NS + FCS                                 */
    LlcPdu_BL_UDATA_FCS  = 0x6,  /* broadcast + FCS                          */
    LlcPdu_BL_ACK_FCS    = 0x7,  /* NR + FCS                                 */
    LlcPdu_AL_SETUP      = 0x8,  /* DL#727 pre-reply wrapper (no MM body)    */
    LlcPdu_Unknown       = 0xFF
} LlcPduType;

/* llc_pdu_type_is_valid — true if the 4-bit value is one we encode/decode. */
static inline bool llc_pdu_type_is_valid(uint8_t v)
{
    return v == LlcPdu_BL_ADATA     || v == LlcPdu_BL_DATA      ||
           v == LlcPdu_BL_UDATA     || v == LlcPdu_BL_ACK       ||
           v == LlcPdu_BL_ADATA_FCS || v == LlcPdu_BL_DATA_FCS  ||
           v == LlcPdu_BL_UDATA_FCS || v == LlcPdu_BL_ACK_FCS   ||
           v == LlcPdu_AL_SETUP;
}

/* llc_pdu_type_has_fcs — true if the type's encoding includes a 32-bit FCS
 * after the body. Mirrors the +4 offset between BL-ADATA / BL-ADATA+FCS. */
static inline bool llc_pdu_type_has_fcs(LlcPduType t)
{
    return ((unsigned) t & 0x4u) != 0u && (unsigned) t < 0x8u;
}

/* llc_pdu_type_has_nr — true if NR is part of the on-air encoding. */
static inline bool llc_pdu_type_has_nr(LlcPduType t)
{
    switch (t) {
    case LlcPdu_BL_ADATA: case LlcPdu_BL_ADATA_FCS:
    case LlcPdu_BL_ACK:   case LlcPdu_BL_ACK_FCS:
        return true;
    default:
        return false;
    }
}

/* llc_pdu_type_has_ns — true if NS is part of the on-air encoding. */
static inline bool llc_pdu_type_has_ns(LlcPduType t)
{
    switch (t) {
    case LlcPdu_BL_ADATA: case LlcPdu_BL_ADATA_FCS:
    case LlcPdu_BL_DATA:  case LlcPdu_BL_DATA_FCS:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * LlcPdu — decoded view of a single LLC PDU.
 *
 * `body` points into a caller-owned bit buffer carrying MLE-PD + MM body
 * (or whatever the upper layer carries — LLC is payload-agnostic). Stored
 * MSB-first packed into bytes; `body_len_bits` is the exact bit-length.
 * For AL-SETUP, body_len_bits == 0 (no body, just wrapper).
 * ------------------------------------------------------------------------- */
#define LLC_PDU_BODY_MAX_BYTES 32u   /* 256 bits — > any single-burst SCH/F */

typedef struct {
    LlcPduType pdu_type;
    uint8_t    nr;            /* 0/1 — only valid if has_nr(pdu_type)       */
    uint8_t    ns;            /* 0/1 — only valid if has_ns(pdu_type)       */
    uint16_t   body_len_bits; /* MLE-PD + upper-layer body in bits          */
    uint8_t    body[LLC_PDU_BODY_MAX_BYTES];
    uint32_t   fcs;           /* only valid if has_fcs(pdu_type)            */
    bool       fcs_valid;     /* set by decode if FCS check passed          */
} LlcPdu;

/* ---------------------------------------------------------------------------
 * LLC PDU encode/decode (bit-exact on-air).
 *
 * llc_pdu_encode: writes the LLC PDU into `out` starting at the cursor.
 *   Layout per type (all MSB-first):
 *     pdu_type            4 bits
 *     N(R)                1 bit  (if has_nr)
 *     N(S)                1 bit  (if has_ns)
 *     body                body_len_bits
 *     FCS                 32 bits (if has_fcs) — CRC-32 over (pdu_type ..
 *                                                              body)
 *   Returns number of bits written, or -EINVAL on bad args.
 *
 * llc_pdu_decode: reads from `in` starting at the cursor. `body_len_bits`
 *   in `out` MUST be set by the caller before calling — the LLC frame
 *   has no length self-description on-air; bluestation passes the
 *   remaining-bits hint from MAC. Returns 0 on success, negative on
 *   error (-EINVAL bad args, -EPROTO short-frame).
 *
 * For AL-SETUP both encode and decode skip the body entirely and set
 * body_len_bits = 0. NR/NS are not encoded.
 * ------------------------------------------------------------------------- */
int  llc_pdu_encode(BitBuffer *out, const LlcPdu *pdu);
int  llc_pdu_decode(BitBuffer *in, LlcPdu *out);

/* ---------------------------------------------------------------------------
 * CRC-32 helper (LLC FCS).
 *
 * Polynomial = 0x04C11DB7 (IEEE 802.3 / "CRC-32"), reflected input/output,
 * init = 0xFFFFFFFF, xorout = 0xFFFFFFFF. This is the de-facto Ethernet
 * CRC-32 and is the most-cited choice when ETSI sources name "CRC-32"
 * without further qualification.
 *
 *   <-- TODO: confirm polynomial against ETSI EN 300 392-2 §22 -->
 * The current ETSI PDFs in docs/references/ are §9 (suppl. services) and
 * the V+D designers' guide which does not fix the LLC FCS polynomial.
 * Until the §22 PDF is added, FCS verification is round-trip-only:
 * encode→decode→encode produces identical bits, but we do NOT have a
 * Gold-Ref FCS bit-vector to test against (DL#735 BL-ADATA in the M2
 * capture is the no-FCS variant; UL#0/UL#2 are likewise no-FCS).
 *
 * Input: arbitrary-length MSB-first bit buffer over `data` of length
 * `len_bits`. Output: 32-bit CRC, ready to be appended MSB-first.
 * ------------------------------------------------------------------------- */
uint32_t llc_crc32(const uint8_t *data, size_t len_bits);

/* ---------------------------------------------------------------------------
 * LLC entity — stop-and-wait per endpoint.
 *
 * Per ETSI EN 300 392-2 §22: BL-class operates with single-bit NR/NS
 * (modulo-2). Each endpoint maintains independent send/receive states.
 *
 *   ns_send       next NS to put on the wire when transmitting BL-DATA/
 *                  BL-ADATA. Toggles 0↔1 on each successful send.
 *   nr_expected   next NS we expect to receive on a BL-(A)DATA. When the
 *                  peer sends NS == nr_expected we ACK it and toggle.
 *                  When NS != nr_expected the peer is retransmitting and
 *                  we re-ACK the previous (do NOT advance nr_expected).
 *   awaiting_ack  true while a transmitted BL-(A)DATA is unacknowledged.
 *   retx_count    number of retransmissions of the current send-frame.
 *
 * ------------------------------------------------------------------------- */
#define LLC_MAX_ENDPOINTS 8u

typedef struct {
    EndpointId id;
    bool       in_use;
    uint8_t    ns_send;
    uint8_t    nr_expected;
    bool       awaiting_ack;
    uint8_t    retx_count;
    /* cached last-sent body so retransmission resends bit-identical bits */
    LlcPdu     last_sent;
} LlcEndpoint;

/* LlcCfg — caller-supplied config + storage shape. */
typedef struct {
    /* Maximum allowed retransmissions before declaring link broken. ETSI
     * §22 leaves this an MS-side timer; bluestation defaults to 3. */
    uint8_t  max_retx;
} LlcCfg;

#define LLC_DEFAULT_MAX_RETX 3u

/* LlcStats — observable counters for tests + WebUI. */
typedef struct {
    size_t pdus_rx;
    size_t pdus_tx;
    size_t fcs_failures;
    size_t retx_total;
    size_t unknown_pdu_type;
} LlcStats;

/* Llc — entity instance. */
typedef struct {
    MsgBus      *bus;
    LlcCfg       cfg;
    LlcEndpoint  endpoints[LLC_MAX_ENDPOINTS];
    LlcStats     stats;
    bool         initialised;
} Llc;

/* ---------------------------------------------------------------------------
 * TmaUnitdataInd — TMA-SAP RX primitive (MAC → LLC).
 *
 * Carries an opaque TM-SDU bit-buffer as observed on-air. Body MSB-first,
 * length in bits. The MAC layer does NOT pre-parse the LLC PDU — that's
 * what we do here.
 *
 * `ssi`/`ssi_type` reproduce the MAC-RESOURCE / MAC-ACCESS header
 * addressing so LLC can route to the right endpoint without re-parsing
 * the MAC header. `endpoint` is 1..NUM_TIMESLOTS for traffic channels;
 * for signalling the upper layers manage their own endpoint mapping.
 * ------------------------------------------------------------------------- */
#define TMA_SDU_MAX_BYTES 64u   /* 512 bits — > SCH/F payload after MAC hdr */

typedef struct {
    EndpointId   endpoint;
    TetraAddress addr;
    ReqHandle    req_handle;     /* SAP-internal correlator (tetra/types.h).
                                  * 0 = REQ_HANDLE_NONE (not requested);
                                  * else allocated by req_handle_next() at
                                  * emit-time, returned in the matching
                                  * TmaReportInd. Closes Phase-3.7 P5. */
    uint16_t     sdu_len_bits;
    uint8_t      sdu_bits[TMA_SDU_MAX_BYTES];
} TmaUnitdataInd;

/* TmaUnitdataReq — TMA-SAP TX primitive (LLC → MAC). Same shape. */
typedef TmaUnitdataInd TmaUnitdataReq;

/* TleSapMsg — LLC ↔ MLE message envelope (TleSap, ETSI §20.4).
 *
 * Carries a decoded LLC PDU upward to MLE. The MLE-PD field at the start
 * of the LLC body is part of `pdu.body` — LLC is payload-agnostic and
 * does not pre-strip MLE-PD. Same struct used downward (MLE → LLC).
 *
 * `addr` is propagated so MLE can route by ISSI without knowing TmaSap. */
typedef struct {
    EndpointId   endpoint;
    TetraAddress addr;
    LlcPdu       pdu;
} TleSapMsg;

/* ---------------------------------------------------------------------------
 * LLC entity API — locked under IF_LLC_v1.
 *
 * llc_init: zero-init entity, register handlers on the bus. `cfg` may be
 *   NULL → defaults applied. `bus` MUST be initialised. Returns 0 on
 *   success, -EINVAL on bad args.
 *
 * llc_handle_tma_unitdata_ind: parse TMA-SDU bits as an LLC PDU, update
 *   per-endpoint NR/NS state, ACK incoming BL-(A)DATA via the bus,
 *   forward the decoded PDU to MLE via TleSap. Returns 0 on success,
 *   -EPROTO on parse error.
 *
 * llc_post_to_mle: post a TleSapMsg upward via the bus at the SAP's
 *   default priority. Used internally; exposed for tests.
 *
 * llc_endpoint_lookup: returns pointer to the endpoint state for `id`.
 *   Allocates a free slot if not present. Returns NULL if all slots
 *   are full. (Public for tests; production callers go through the
 *   handle_tma_unitdata_ind path.)
 *
 * llc_handle_tma_unitdata_ind is the canonical MAC→LLC entry; for
 * MLE→LLC traffic, MLE posts a TleSapMsg via msgbus and the handler
 * registered by llc_init() picks it up.
 * ------------------------------------------------------------------------- */
int          llc_init(Llc *llc, MsgBus *bus, const LlcCfg *cfg);
int          llc_handle_tma_unitdata_ind(Llc *llc, const TmaUnitdataInd *ind);
int          llc_post_to_mle(Llc *llc, const TleSapMsg *msg);
LlcEndpoint *llc_endpoint_lookup(Llc *llc, EndpointId id);

/* llc_send_bl_data — transmit a BL-DATA on `endpoint`, advancing NS,
 * stashing for retransmit. Returns 0 on success, -ENOSPC if no endpoint
 * slot, -EBUSY if awaiting_ack already true, -EINVAL on bad args. */
int llc_send_bl_data(Llc *llc, EndpointId endpoint, const TetraAddress *addr,
                     const uint8_t *body, uint16_t body_len_bits);

/* llc_send_bl_ack — transmit a BL-ACK with NR=ep->nr_expected. Used
 * after a successful BL-(A)DATA RX. Returns 0/error as above. */
int llc_send_bl_ack(Llc *llc, EndpointId endpoint, const TetraAddress *addr);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_LLC_H */

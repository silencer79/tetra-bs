/* sw/cmce/include/tetra/cmce.h — CMCE public API.
 *
 * Owned by S4 (S4-sw-cmce). Locked under interface contract IF_CMCE_v1
 * (per docs/MIGRATION_PLAN.md §"Agent Topology" §S4 + §"Interface-locking
 * schedule").
 *
 * Circuit Mode Control Entity per ETSI EN 300 392-2 §14 + bluestation
 * `cmce/`. Provides:
 *   - PDU encode/decode for U/D-SETUP, D-CALL-PROCEEDING, D-CONNECT,
 *     U-TX-DEMAND, D-TX-GRANTED, U/D-RELEASE, D-NWRK-BROADCAST
 *   - Per-call FSM (SETUP_PENDING, PROCEEDING, CONNECTED, TX_GRANTED,
 *     RELEASING, IDLE), one slot per active call_identifier
 *   - Periodic D-NWRK-BCAST emitter at 10s cadence per Gold-Ref Burst #423
 *     (see docs/references/reference_gold_full_attach_timeline.md
 *     §"D-NWRK-BROADCAST-Cadence" — 10.0s ± 30ms over 10 captured bursts).
 *
 * Source-of-truth hierarchy (CLAUDE.md §1): Gold > Bluestation > ETSI.
 *   - D-NWRK-BCAST: backed by GOLD_INFO_124 from
 *     scripts/gen_d_nwrk_broadcast.py (Gold Burst #423). Conservative
 *     encoder default = `tetra_network_time = None` (o-bit=0), per
 *     gold_field_values.md §"D-NWRK-BCAST `tetra_network_time` ... open
 *     uncertainty" recommendation.
 *   - All other CMCE PDUs: PROVISIONAL — bit layouts come from
 *     reference_cmce_group_call_pdus.md (bluestation+ETSI §14.7), NOT
 *     Gold-Ref. Phase G/4 will harden these against a real Group-Call
 *     capture (gold_field_values.md "Open uncertainties" #5).
 *
 * Pure header (no .c). Includes msgbus + types from S0 + llc.h from S2 for
 * the TleSapMsg envelope used to push LLC-body bits downward.
 */
#ifndef TETRA_CMCE_H
#define TETRA_CMCE_H

#include "tetra/llc.h"
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
 * CmcePduType — 5-bit DL/UL discriminator per ETSI §14.8.28 +
 * reference_cmce_group_call_pdus.md.
 *
 * The on-air encoding for a given direction (DL vs UL) keys off the *same*
 * 5-bit value; semantics are direction-dependent (e.g. code 7 = D-SETUP on
 * DL, U-SETUP on UL — same bits, different state-machine consumers).
 * Naming here matches bluestation `cmce/pdus/`.
 *
 * Values not in this table decode to CmcePdu_Unknown so unknown PDUs do
 * not leak into the FSM as if they were known.
 * ------------------------------------------------------------------------- */
typedef enum {
    /* Shared codes — same wire-value, different direction = different PDU */
    CmcePdu_Setup            = 7,   /* D-SETUP   DL  / U-SETUP UL          */
    CmcePdu_CallProceeding   = 1,   /* D-CALL-PROCEEDING (DL only)         */
    CmcePdu_Connect          = 2,   /* D-CONNECT (DL)                      */
    CmcePdu_TxDemand         = 10,  /* U-TX-DEMAND (UL)                    */
    CmcePdu_TxGranted        = 11,  /* D-TX-GRANTED (DL)                   */
    CmcePdu_Release          = 6,   /* U/D-RELEASE                         */
    /* D-NWRK-BCAST is NOT an entry in §14.8.28 — it is an MLE PDU, not a
     * CMCE PDU. We carry its build/parse here because the periodic 10s
     * driver lives in this entity (per Agent contract). It does not need
     * a 5-bit type code (it goes through MLE-prim=2). */
    CmcePdu_NwrkBroadcast    = 0xFE,
    CmcePdu_Unknown          = 0xFF
} CmcePduType;

/* Direction discriminator — DL = BS→MS, UL = MS→BS. Used as a hint to the
 * codec because CmcePdu_Setup means D-SETUP on DL and U-SETUP on UL. */
typedef enum {
    CmceDir_Downlink = 0,
    CmceDir_Uplink   = 1
} CmceDirection;

/* ---------------------------------------------------------------------------
 * Common CMCE field types.
 *
 * Bit-widths per ETSI §14.8 + bluestation `cmce/pdus/common_*`.
 * ------------------------------------------------------------------------- */

/* TransmissionGrant — 2 bit, §14.8.42. */
typedef enum {
    CmceTxGrant_Granted             = 0,
    CmceTxGrant_NotGranted          = 1,
    CmceTxGrant_RequestQueued       = 2,
    CmceTxGrant_GrantedToOtherUser  = 3
} CmceTransmissionGrant;

/* DisconnectCause — 5 bit, §14.8.13. We name a couple representative
 * values; the wire is just a 5-bit number, no enum-strict checking. */
typedef enum {
    CmceDisconnect_UserRequested    = 0,
    CmceDisconnect_NoChannelAvail   = 8,
    CmceDisconnect_CallTimeout      = 12,
    CmceDisconnect_NetworkBusy      = 17,
    CmceDisconnect_Unknown          = 31  /* sentinel for parser-misses    */
} CmceDisconnectCause;

/* PartyTypeIdentifier — 2 bit, §14.8.32. */
typedef enum {
    CmcePty_Sna      = 0,
    CmcePty_Ssi      = 1,
    CmcePty_Tsi      = 2,
    CmcePty_Reserved = 3
} CmcePartyType;

/* BasicServiceInformation — 8 bit, §14.8.2. We carry the raw 8-bit value
 * to keep the codec compact; bit-layout per
 * reference_cmce_group_call_pdus.md §"BasicServiceInformation". */
typedef uint8_t CmceBasicService;

/* Convenience packer for BasicServiceInformation — circuit_mode_type=TchS
 * + speech_service=TetraEncoded + clear text + p2p is what the M3
 * group-call test path uses. The raw 8-bit field stays the canonical
 * representation; this is just a builder for tests + internal callers. */
static inline CmceBasicService cmce_bsi_make(uint8_t circuit_mode_type,
                                             uint8_t encryption_flag,
                                             uint8_t communication_type,
                                             uint8_t variant_field)
{
    return (CmceBasicService) (
        ((circuit_mode_type    & 0x7u) << 5) |
        ((encryption_flag      & 0x1u) << 4) |
        ((communication_type   & 0x3u) << 2) |
        ((variant_field        & 0x3u) << 0));
}

/* ---------------------------------------------------------------------------
 * CmcePdu — decoded view of a CMCE PDU (any direction, any type).
 *
 * Only the fields relevant to the encoded `pdu_type` are valid. The
 * decoder zeroes the struct then fills the relevant subset; encoder
 * reads only the relevant subset.
 *
 * Bit-widths and field positions per
 * docs/references/reference_cmce_group_call_pdus.md.
 * ------------------------------------------------------------------------- */
#define CMCE_PDU_BODY_MAX_BYTES 32u  /* 256 bits — > any single-burst CMCE  */

typedef struct {
    CmcePduType    pdu_type;

    /* Shared field carried by Setup/Proceeding/Connect/TxGranted/Release */
    uint16_t       call_identifier;   /* 14 bit                            */

    /* D-SETUP / D-CONNECT */
    uint8_t        call_time_out;     /* 4 bit (CallTimeout)               */
    uint8_t        call_time_out_set_up_phase;  /* 3 bit (D-CALL-PROC)     */
    uint8_t        hook_method_selection;       /* 1 bit                   */
    uint8_t        simplex_duplex_selection;    /* 1 bit                   */
    CmceBasicService basic_service_information; /* 8 bit                   */
    CmceTransmissionGrant transmission_grant;   /* 2 bit                   */
    uint8_t        transmission_request_permission; /* 1 bit               */
    uint8_t        call_priority;     /* 4 bit                             */
    uint8_t        call_ownership;    /* 1 bit (D-CONNECT)                 */

    /* U-SETUP */
    uint8_t        area_selection;          /* 4 bit                       */
    uint8_t        request_to_transmit_send_data; /* 1 bit                 */
    uint8_t        clir_control;            /* 2 bit                       */
    CmcePartyType  called_party_type_identifier; /* 2 bit                  */
    uint32_t       called_party_address_ssi;     /* 24 bit (Ssi/Tsi)       */
    uint8_t        called_party_short_number_address; /* 8 bit (Sna)       */

    /* U-TX-DEMAND / D-TX-GRANTED */
    uint8_t        tx_demand_priority;     /* 2 bit                        */
    uint8_t        encryption_control;     /* 1 bit                        */

    /* U/D-RELEASE */
    CmceDisconnectCause disconnect_cause;  /* 5 bit                        */

    /* D-NWRK-BCAST body — see §"D-NWRK-BCAST" below */
    uint16_t       nwrk_cell_re_select_parameters; /* 16 bit               */
    uint8_t        nwrk_cell_load_ca;              /*  2 bit               */

    /* Optional-fields presence — encoder/decoder set this when the o-bit
     * is processed. For D-NWRK-BCAST: Gold #423 has o-bit=1 (TNT + NCA
     * present); see gold_field_values.md §"mle/D-NWRK-BROADCAST" for the
     * full ETSI Table 18.100 TNT layout. */
    bool           optionals_present;      /* 1 bit (the "o-bit")          */

    /* D-NWRK-BCAST optional fields (only valid when optionals_present).
     * Reset-default values are taken from Gold #423 decode (2026-05-03);
     * the encoder uses these verbatim unless the caller overrides. */
    bool           nwrk_p_tetra_network_time;     /* 1 bit p-bit              */
    uint64_t       nwrk_tetra_network_time;       /* 48 bit per ETSI §18.5.24 */
    bool           nwrk_p_num_ca_neighbour_cells; /* 1 bit p-bit              */
    uint8_t        nwrk_num_ca_neighbour_cells;   /* 3 bit count              */

    /* Total encoded length in bits — decoder fills this; encoder returns
     * it as the int return value too. */
    uint16_t       encoded_len_bits;
} CmcePdu;

/* ---------------------------------------------------------------------------
 * CMCE PDU encode/decode.
 *
 * cmce_pdu_encode: write the CMCE PDU at bb's cursor in the layout per
 *   reference_cmce_group_call_pdus.md for `dir`. Returns number of bits
 *   written, negative errno on bad args.
 *
 * cmce_pdu_decode: read at bb's cursor, fill `out`. The remaining-bits
 *   hint comes from the LLC body length (callers know this from the MAC
 *   length-indication). Returns 0 on success, -EPROTO on short-frame.
 *
 * Direction-keying:
 *   - D-SETUP / U-SETUP share `pdu_type=7`; the codec switches on `dir`.
 *   - U-RELEASE / D-RELEASE share `pdu_type=6`; codec switches on `dir`
 *     (the on-air bit-layout is identical per §14.7.x.9 — the direction
 *     just changes which FSM consumes it).
 *
 * D-NWRK-BCAST is encoded by cmce_pdu_encode_d_nwrk_broadcast (separate
 * entry point — see below) because it does not have a CMCE pdu_type
 * field on-air; it is an MLE-PD=5 / mle_prim=2 PDU.
 * ------------------------------------------------------------------------- */
int cmce_pdu_encode(BitBuffer *out, const CmcePdu *pdu, CmceDirection dir);
int cmce_pdu_decode(BitBuffer *in,  CmcePdu *out,       CmceDirection dir,
                    uint16_t in_len_bits);

/* cmce_pdu_encode_d_nwrk_broadcast — encode the D-NWRK-BROADCAST MLE body
 * starting at bb's cursor.
 *
 * Conservative-encoder default per gold_field_values.md §"Konservativer
 * Default für unseren Encoder":
 *   - o-bit = 0
 *   - tetra_network_time absent
 *   - p_number_of_ca_neighbour_cells = 0
 *   ⇒ body = cell_re_select_parameters[16] | cell_load_ca[2] | o-bit[1]
 *          = 19 bits (then padded by the caller to LI=16 octets = 128 bit
 *            on-air via fill bits in the MAC-RESOURCE wrapper).
 *
 * If `pdu->optionals_present` is true the encoder will refuse and return
 * -ENOTSUP — the open uncertainty in gold_field_values.md §"Open
 * uncertainties" #2 means we do not know the full TNT bit-allocation in
 * Gold #423, so we hard-stop rather than emit guessed bits.
 *
 * Returns number of bits written, or negative errno. */
int cmce_pdu_encode_d_nwrk_broadcast(BitBuffer *out, const CmcePdu *pdu);

/* cmce_pdu_decode_d_nwrk_broadcast — symmetric decoder. Reads at bb's
 * cursor, fills `out->nwrk_*` + sets `out->optionals_present`. Returns
 * number of bits consumed, negative on error. */
int cmce_pdu_decode_d_nwrk_broadcast(BitBuffer *in, CmcePdu *out,
                                     uint16_t in_len_bits);

/* ---------------------------------------------------------------------------
 * CMCE per-call FSM.
 *
 * Per-call_id state machine. State transitions per
 * reference_cmce_group_call_pdus.md §"Sequenz-Diagramme" + ETSI §14.6.x.
 *
 * Slot life-cycle:
 *   IDLE -> SETUP_PENDING (on U-SETUP rx OR explicit BS-initiated trigger)
 *   SETUP_PENDING -> PROCEEDING (D-CALL-PROCEEDING sent)
 *   PROCEEDING    -> CONNECTED  (D-CONNECT sent + ACKed)
 *   CONNECTED     -> TX_GRANTED (U-TX-DEMAND rx + D-TX-GRANTED sent)
 *   TX_GRANTED    -> CONNECTED  (U-TX-CEASED rx — voice-burst phase end)
 *   CONNECTED     -> RELEASING  (U/D-RELEASE sent or rx)
 *   RELEASING     -> IDLE       (release ack rx OR timeout)
 * ------------------------------------------------------------------------- */
typedef enum {
    CmceCall_Idle           = 0,
    CmceCall_SetupPending   = 1,
    CmceCall_Proceeding     = 2,
    CmceCall_Connected      = 3,
    CmceCall_TxGranted      = 4,
    CmceCall_Releasing      = 5
} CmceCallState;

#define CMCE_MAX_CALLS 8u

typedef struct {
    bool           in_use;
    uint16_t       call_identifier;     /* 14 bit on-air                   */
    CmceCallState  state;
    TetraAddress   peer_addr;           /* called/calling party SSI/GSSI    */
    EndpointId     endpoint;            /* slot/timeslot for voice          */
    uint8_t        retry_count;         /* setup-phase retries              */
} CmceCallSlot;

/* CmceCfg — caller-supplied tunables. */
typedef struct {
    /* Multiframe-tick interval at which the periodic-driver fires its
     * D-NWRK-BCAST. Gold cadence = 10s = ~16.7 multiframes (1 multiframe
     * = ~600ms in TETRA — 18 frames * 56.67ms). We measure the tick in
     * multiframes here because the daemon main loop ticks once per
     * multiframe (S7 contract). */
    uint16_t  nwrk_bcast_period_multiframes;

    /* MCC / MNC / CC for D-NWRK-BCAST cell_re_select_parameters seed —
     * tests pass these through unchanged. Gold-Cell defaults shown. */
    uint16_t  cell_re_select_parameters_seed;  /* default 0x5655           */
    uint8_t   cell_load_ca;                    /* default 0 (low load)     */
} CmceCfg;

/* Default period: 10s / (18 frames * 56.67ms) = 9.81 → round to 10
 * multiframes, accepting <0.5% jitter vs Gold. The exact value can be
 * overridden via cfg.nwrk_bcast_period_multiframes. */
#define CMCE_NWRK_BCAST_PERIOD_MF_DEFAULT 10u  /* 10 multiframes * 1.02s ≈ 10.2s, matches Gold cadence */

#define CMCE_NWRK_DEFAULT_CRSP   0x5655u  /* gold #423 cell_re_select_params*/
#define CMCE_NWRK_DEFAULT_CL_CA  0x0u     /* gold #423 cell_load_ca         */

/* CmceStats — observable counters. */
typedef struct {
    size_t pdus_rx;
    size_t pdus_tx;
    size_t setup_count;
    size_t connect_count;
    size_t tx_grant_count;
    size_t release_count;
    size_t nwrk_bcast_count;
    size_t unknown_pdu_type;
    size_t fsm_drops;     /* messages dropped because no slot or bad state */
} CmceStats;

/* ---------------------------------------------------------------------------
 * CMCE entity instance.
 *
 * Multi-call (CMCE_MAX_CALLS slots). Single-thread, single-MsgBus.
 * The periodic-driver state lives here so cmce_send_d_nwrk_broadcast can
 * be called by the daemon's main timer without re-entering CMCE state.
 * ------------------------------------------------------------------------- */
typedef struct {
    MsgBus        *bus;
    CmceCfg        cfg;
    CmceCallSlot   calls[CMCE_MAX_CALLS];
    CmceStats      stats;
    /* Periodic-driver state (cmce_nwrk_bcast.c). */
    uint64_t       last_bcast_tick_mf;
    bool           initialised;
} Cmce;

/* ---------------------------------------------------------------------------
 * CMCE entity API — locked under IF_CMCE_v1.
 *
 * cmce_init: zero-init entity, register handlers on the bus. `cfg` may be
 *   NULL → defaults applied. `bus` MUST be initialised. Returns 0 on
 *   success, -EINVAL on bad args.
 *
 * cmce_send_d_nwrk_broadcast: build + emit a D-NWRK-BROADCAST PDU
 *   downward via TleSap. Called by the daemon scheduler at the 10s
 *   cadence (or whenever cmce_nwrk_bcast_tick says it is due). Returns 0
 *   on success, negative errno on encode/post failure.
 *
 * cmce_handle_tle_msg: ingest a TleSapMsg from MLE (or LLC). Updates the
 *   per-call FSM and may emit one or more downstream TleSapMsgs. Returns
 *   0 on success, -EPROTO on parse error, -ENOSPC if no call slot
 *   available for a new call.
 *
 * cmce_post_tma_unitdata_req: variadic-style helper to post a TmaSap
 *   unit-data-req carrying a CMCE PDU encoded into LLC body. Used by the
 *   FSM internally; exposed for tests. Returns 0 on success, negative on
 *   error. (Note: takes `EndpointId` + addr instead of va_args because
 *   the IF_CMCE_v1 lock pinned the spelling but C does not need real
 *   varargs; the trailing-`...` in the contract reflects the agent-spec
 *   prose, not a literal va_list.)
 * ------------------------------------------------------------------------- */
int cmce_init(Cmce *cmce, MsgBus *bus, const CmceCfg *cfg);
int cmce_send_d_nwrk_broadcast(Cmce *cmce);
int cmce_handle_tle_msg(Cmce *cmce, const TleSapMsg *msg);
int cmce_post_tma_unitdata_req(Cmce *cmce,
                               EndpointId endpoint,
                               const TetraAddress *addr,
                               const CmcePdu *pdu,
                               CmceDirection dir);

/* ---------------------------------------------------------------------------
 * FSM helpers (cmce_fsm.c) — public for tests + WebUI introspection.
 * ------------------------------------------------------------------------- */

/* cmce_call_lookup — find call slot by call_identifier; NULL if absent. */
CmceCallSlot *cmce_call_lookup(Cmce *cmce, uint16_t call_identifier);

/* cmce_call_alloc — allocate a free slot for `call_identifier`; NULL if
 * the table is full. Initialises state to SetupPending. */
CmceCallSlot *cmce_call_alloc(Cmce *cmce, uint16_t call_identifier,
                              const TetraAddress *peer, EndpointId endpoint);

/* cmce_call_release — set state to Idle and free the slot. */
void cmce_call_release(Cmce *cmce, CmceCallSlot *slot);

/* cmce_fsm_apply — drive the FSM given an incoming PDU. Returns the new
 * state (or CmceCall_Idle if the slot was freed). */
CmceCallState cmce_fsm_apply(Cmce *cmce, CmceCallSlot *slot,
                             const CmcePdu *pdu, CmceDirection dir);

/* ---------------------------------------------------------------------------
 * Periodic-driver hook (cmce_nwrk_bcast.c).
 *
 * cmce_nwrk_bcast_tick: pure tick-evaluator — given the daemon's current
 * multiframe counter, decide whether the periodic-driver should fire NOW.
 * Returns true if cmce_send_d_nwrk_broadcast should be called by the
 * scheduler; the function then updates the entity's last_bcast_tick_mf
 * to the supplied `now_mf` (so the caller need not).
 *
 * Design note: this function is the entire interface between the
 * periodic driver and the daemon main loop — passing the multiframe
 * counter in keeps the driver test-deterministic without a real clock.
 * ------------------------------------------------------------------------- */
bool cmce_nwrk_bcast_tick(Cmce *cmce, uint64_t now_mf);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_CMCE_H */

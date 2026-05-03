/* sw/core/include/tetra/types.h — Common SW-side TETRA types.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Locked under interface contract
 * IF_CORE_API_v1. Bit-widths come from
 * docs/references/gold_field_values.md (TODO-A); structural template is
 * tetra-bluestation/crates/tetra-core/src/. Source-of-truth hierarchy
 * Gold > Bluestation > ETSI per CLAUDE.md §1.
 *
 * Self-contained: pulls in stdint.h / stdbool.h / stddef.h. No external
 * project deps allowed in this header — downstream agents include it
 * directly without further plumbing.
 */
#ifndef TETRA_CORE_TYPES_H
#define TETRA_CORE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * SsiType — TETRA Short Subscriber Identity discriminator.
 *
 * Bit-width on-air (when serialised in MAC headers): 3 bits per
 * gold_field_values.md §"TmaSap" / address_type field. The 3-bit raw
 * encoding follows ETSI EN 300 392-2 §16.10.5 Table 16.103.
 * Values not in that table are decoded to SsiType_Unknown.
 * ------------------------------------------------------------------------- */
typedef enum {
    SsiType_Unknown    = 0,
    SsiType_Ssi        = 1,  /* generic SSI when subtype unknown            */
    SsiType_Issi       = 2,  /* Individual Short Subscriber Identity        */
    SsiType_Gssi       = 3,  /* Group Short Subscriber Identity             */
    SsiType_Ussi       = 4,  /* Unexchanged SSI                             */
    SsiType_Smi        = 5,  /* Short Management Identity                   */
    SsiType_Esi        = 6,  /* Encrypted SSI                               */
    SsiType_EventLabel = 7   /* Umac-internal event-label tag               */
} SsiType;

/* ---------------------------------------------------------------------------
 * TetraAddress — SSI + type discriminator.
 *
 * ssi      = 24-bit on-air (gold_field_values.md M2/M3 captures, e.g.
 *            ITSI=0x282FF4, GSSI=0x2F4D61). Stored as uint32_t with the
 *            low 24 bits used; upper 8 bits MUST be zero.
 * ssi_type = 3-bit discriminator (see SsiType above).
 *
 * address_extension is carried separately in TmaUnitdataInd /
 * TmaUnitdataReq when present; per gold_field_values.md it is
 * 24-bit on-air, also stored low-24-bits in uint32_t.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t ssi;       /* 24-bit, MSB byte must be 0                       */
    SsiType  ssi_type;  /* 3-bit on-air encoding                            */
} TetraAddress;

#define TETRA_SSI_MASK_24 ((uint32_t) 0x00FFFFFFu)

/* ---------------------------------------------------------------------------
 * EndpointId — MLE↔LLC↔MAC endpoint handle.
 *
 * 32-bit per gold_field_values.md §"TmaSap" (uses bluestation `EndpointId =
 * u32`). Currently encodes the timeslot number 1..4 used by the MAC, but
 * the SAP-internal type is 32 bits to keep room for future namespacing.
 * ------------------------------------------------------------------------- */
typedef uint32_t EndpointId;

/* ---------------------------------------------------------------------------
 * TdmaTime — TETRA TDMA frame coordinate (Clause 7.6 + bluestation
 * tetra-core/src/tdma_time.rs).
 *
 *   t  timeslot       1..4
 *   f  frame number   1..18
 *   m  multiframe     1..60
 *   h  hyperframe     0..0xFFFF
 *
 * Defaults to (0/1/1/1) — see TDMA_TIME_DEFAULT below.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t  t;
    uint8_t  f;
    uint8_t  m;
    uint16_t h;
} TdmaTime;

#define TDMA_TIME_DEFAULT ((TdmaTime){ .t = 1, .f = 1, .m = 1, .h = 0 })

/* ---------------------------------------------------------------------------
 * BurstType — PHY burst classifications (ETSI Clause 9.4.4.1, mirrored
 * from bluestation phy_types.rs).
 * ------------------------------------------------------------------------- */
typedef enum {
    BurstType_CUB = 0,  /* Control Uplink Burst                             */
    BurstType_NUB = 1,  /* Normal Uplink Burst                              */
    BurstType_NDB = 2,  /* Normal Downlink Burst                            */
    BurstType_SDB = 3   /* Synchronization Downlink Burst                   */
} BurstType;

/* ---------------------------------------------------------------------------
 * TrainingSequence — per ETSI Clause 9.4.4.3 + bluestation phy_types.rs.
 * 0 == NotFound to mirror bluestation default.
 * ------------------------------------------------------------------------- */
typedef enum {
    TrainingSequence_NotFound        = 0,
    TrainingSequence_NormalTrainSeq1 = 1,  /* 22 n-bits                     */
    TrainingSequence_NormalTrainSeq2 = 2,  /* 22 p-bits                     */
    TrainingSequence_NormalTrainSeq3 = 3,  /* 22 q-bits                     */
    TrainingSequence_ExtendedTrainSeq = 4, /* 30 x-bits                     */
    TrainingSequence_SyncTrainSeq    = 5   /* 38 y-bits                     */
} TrainingSequence;

/* ---------------------------------------------------------------------------
 * LogicalChannel — TMV-SAP logical-channel discriminators (ETSI Clause
 * 9.3.4 + bluestation tmv/enums/logical_chans.rs). Values are stable
 * but not bit-exact on-air — they only tag SAP messages.
 * ------------------------------------------------------------------------- */
typedef enum {
    LogicalChannel_AACH  = 0,
    LogicalChannel_BSCH  = 1,
    LogicalChannel_BNCH  = 2,
    LogicalChannel_SCH_F = 3,  /* full-slot signalling                     */
    LogicalChannel_SCH_HD = 4, /* half-slot DL                             */
    LogicalChannel_SCH_HU = 5, /* half-slot UL (MAC-ACCESS)                */
    LogicalChannel_TCH_S = 6,  /* speech (ACELP)                           */
    LogicalChannel_TCH_7_2 = 7,
    LogicalChannel_TCH_4_8 = 8,
    LogicalChannel_TCH_2_4 = 9,
    LogicalChannel_STCH  = 10,
    LogicalChannel_Unknown = 0xFF
} LogicalChannel;

/* ---------------------------------------------------------------------------
 * PhysicalChannel — TMV-SAP physical-channel kind (bluestation
 * sap_fields.rs). Tp = traffic, Cp = control, Unallocated = neither.
 * ------------------------------------------------------------------------- */
typedef enum {
    PhysicalChannel_Tp          = 0,
    PhysicalChannel_Cp          = 1,
    PhysicalChannel_Unallocated = 2
} PhysicalChannel;

/* ---------------------------------------------------------------------------
 * BitBuffer — MSB-first bit-cursor over a byte array.
 *
 * Mirrors bluestation tetra-core/src/bitbuffer.rs (subset relevant for
 * SW-side encode/decode). `buffer` is non-owning: caller supplies and
 * keeps the storage alive for the lifetime of the BitBuffer (or until
 * autoexpand grows past `cap_bits`, in which case the buffer pointer
 * stays the same — this implementation does NOT realloc; autoexpand only
 * shifts `end` up to `cap_bits`).
 *
 *   buffer    base byte pointer
 *   cap_bits  hard upper bound (= 8 * underlying byte storage)
 *   start     window-start in bits (inclusive, normally 0)
 *   pos       cursor in bits, in [start..end]
 *   end       window-end in bits (exclusive)
 *   autoexpand if true, writes past `end` extend `end` up to `cap_bits`
 *
 * The struct is exposed (not opaque) so downstream code can stack-
 * allocate plus inline-init via bb_init(). Direct field manipulation
 * outside bitbuffer.c is discouraged — use the API.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *buffer;
    size_t   cap_bits;
    size_t   start;
    size_t   pos;
    size_t   end;
    bool     autoexpand;
} BitBuffer;

/* ---------------------------------------------------------------------------
 * SapId — Service Access Point identifier.
 *
 * Subset relevant to the SW-side of the FPGA-↔-SW boundary plus the
 * inter-layer SAPs (LLC↔MLE, MLE↔MM, MLE↔CMCE, etc.). Values are stable
 * — once committed, downstream agents lock against them.
 * ------------------------------------------------------------------------- */
typedef enum {
    SapId_None    = 0,
    SapId_TpSap   = 1,   /* PHY/LMAC                                       */
    SapId_TmvSap  = 2,   /* LMAC/UMAC                                      */
    SapId_TmaSap  = 3,   /* UMAC/LLC signalling                            */
    SapId_TmdSap  = 4,   /* UMAC/LLC user-plane (voice TCH)                */
    SapId_TleSap  = 5,   /* LLC/MLE                                        */
    SapId_TlmbSap = 6,   /* LLC/MLE broadcast                              */
    SapId_TlmcSap = 7,   /* LLC/MLE management                             */
    SapId_TnmmSap = 8,   /* MM ↔ User                                      */
    SapId__Max    = 9    /* upper-bound sentinel; not a valid SAP          */
} SapId;

/* ---------------------------------------------------------------------------
 * MsgPriority — three buckets per ARCHITECTURE.md §"Message Bus".
 * High dispatches first, Low last; FIFO within bucket.
 * ------------------------------------------------------------------------- */
typedef enum {
    MsgPrio_High   = 0,  /* Immediate (e.g. AACH-aware Pre-Reply)          */
    MsgPrio_Normal = 1,  /* Standard L3 PDU processing                     */
    MsgPrio_Low    = 2,  /* Background (TTL sweep, persistence flush)      */
    MsgPrio__Count = 3   /* number of priority buckets                     */
} MsgPriority;

/* ---------------------------------------------------------------------------
 * SapMsg — message-bus envelope.
 *
 * src/dest are SapId values to keep routing simple under "single thread,
 * one dispatcher". The (dest, sap) tuple is the registration key, so a
 * handler is keyed by where the message is going AND through which SAP.
 * `payload` is non-owning: msgbus_post() copies it into the queue (see
 * msgbus.h); subscribers receive a queue-internal SapMsg whose payload
 * pointer is valid only for the duration of the dispatch callback.
 *
 * len is in bytes (not bits). Maximum payload size is enforced by
 * MsgBusCfg.max_payload_bytes (msgbus.h).
 * ------------------------------------------------------------------------- */
typedef struct {
    SapId          src;
    SapId          dest;
    SapId          sap;
    uint16_t       len;
    const uint8_t *payload;
} SapMsg;

#endif /* TETRA_CORE_TYPES_H */

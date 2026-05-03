/* sw/mm/include/tetra/mm.h — MM (Mobility Management) public API.
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MM_v1.
 *
 * MM per ETSI EN 300 392-2 §16 + bluestation `mm/`. Provides:
 *   - U-LOCATION-UPDATE-DEMAND IE-Parser (UL: 129-bit reassembled MM body)
 *   - U-ATTACH-DETACH-GROUP-IDENTITY IE-Parser (UL group attach demand)
 *   - D-LOCATION-UPDATE-ACCEPT builder (DL#735, 102-bit MM body, M2 attach)
 *   - D-ATTACH-DETACH-GROUP-IDENTITY-ACK builder (Group-Attach reply,
 *     62-bit MM body)
 *   - MM entity wired to msgbus on TleSap (LLC <-> MLE/MM with MLE-disc=1)
 *
 * Source-of-truth hierarchy (CLAUDE.md §1): Gold > Bluestation > ETSI.
 *  - Field defaults: docs/references/gold_field_values.md
 *  - DL#735 MM body: docs/references/reference_gold_attach_bitexact.md Z.111-152
 *  - Group-Attach DL slices: docs/references/reference_group_attach_bitexact.md
 *  - UL reassembly: docs/references/reference_demand_reassembly_bitexact.md
 */
#ifndef TETRA_MM_H
#define TETRA_MM_H

#include "tetra/db.h"
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
 * MmPduTypeUl / MmPduTypeDl — 4-bit on-air values per ETSI Tab 16.103a/b.
 * ------------------------------------------------------------------------- */
typedef enum {
    MmPduUl_DOtarCmac           = 0,
    MmPduUl_AuthenticationDemand = 1,
    MmPduUl_ULocationUpdateDemand = 2,
    MmPduUl_UMmStatus           = 3,
    MmPduUl_UCkChangeDemand     = 4,
    MmPduUl_UDisableStatus      = 5,
    MmPduUl_UItsiDetach         = 6,
    MmPduUl_UAttachDetachGroupIdentity = 7,
    MmPduUl_FunctionNotSupported = 14
} MmPduTypeUl;

typedef enum {
    MmPduDl_DOtarKsg            = 0,
    MmPduDl_DAuthenticationCommand = 1,
    MmPduDl_DAuthenticationReject = 2,
    MmPduDl_DAuthenticationResponse = 3,
    MmPduDl_DCkChangeCommand    = 4,
    MmPduDl_DLocationUpdateAccept = 5,
    MmPduDl_DLocationUpdateCommand = 6,
    MmPduDl_DLocationUpdateProceeding = 7,
    MmPduDl_DLocationUpdateReject = 8,
    MmPduDl_DMmStatus           = 9,
    MmPduDl_DDisableStatus      = 10,
    MmPduDl_DAttachDetachGroupIdentityAck = 11,
    MmPduDl_DAttachDetachGroupIdentity = 12,
    MmPduDl_FunctionNotSupported = 14
} MmPduTypeDl;

/* ---------------------------------------------------------------------------
 * LocationUpdateType — 3-bit, ETSI EN 300 392-2 §16.10.36.
 * Mirrors bluestation `mm/enums/location_update_type.rs`.
 * ------------------------------------------------------------------------- */
typedef enum {
    LocUpdate_RoamingLocation   = 0,
    LocUpdate_TemporaryRegistration = 1,
    LocUpdate_PeriodicLocation  = 2,
    LocUpdate_ItsiAttach        = 3,
    LocUpdate_CallRestoration   = 4,
    LocUpdate_MigratingLocation = 5,
    LocUpdate_DemandedLocation  = 6,
    LocUpdate_Disabled          = 7
} LocationUpdateType;

/* ---------------------------------------------------------------------------
 * EnergySavingMode — 3-bit, ETSI EN 300 392-2 §16.10.5.
 * Gold-Ref M2: EnergyEconomy1 (=1).
 * ------------------------------------------------------------------------- */
typedef enum {
    EnergySaving_StayAlive      = 0,
    EnergySaving_EnergyEconomy1 = 1,
    EnergySaving_EnergyEconomy2 = 2,
    EnergySaving_EnergyEconomy3 = 3,
    EnergySaving_EnergyEconomy4 = 4,
    EnergySaving_EnergyEconomy5 = 5,
    EnergySaving_EnergyEconomy6 = 6,
    EnergySaving_EnergyEconomy7 = 7
} EnergySavingMode;

/* ---------------------------------------------------------------------------
 * MmType34ElemIdDl/Ul — 4-bit Type-3/Type-4 element identifier (§16.10.51).
 * Mirrors bluestation `mm/enums/type34_elem_id_*.rs`.
 * ------------------------------------------------------------------------- */
typedef enum {
    MmElemDl_DefaultGroupAttachLifetime = 1,
    MmElemDl_NewRegisteredArea  = 2,
    MmElemDl_SecurityDownlink   = 3,
    MmElemDl_GroupReportResponse = 4,
    MmElemDl_GroupIdentityLocationAccept = 5,
    MmElemDl_DmMsAddress        = 6,
    MmElemDl_GroupIdentityDownlink = 7,
    MmElemDl_AuthenticationDownlink = 10,
    MmElemDl_GroupIdentitySecurity = 12,
    MmElemDl_CellTypeControl    = 13,
    MmElemDl_Proprietary        = 15
} MmType34ElemIdDl;

/* Per bluestation `mm/enums/type34_elem_id_ul.rs` (Clause 16.10.39). */
typedef enum {
    MmElemUl_GroupIdentityLocationDemand = 3,
    MmElemUl_GroupReportResponse        = 4,
    MmElemUl_DmMsAddress                = 6,
    MmElemUl_GroupIdentityUplink        = 8,
    MmElemUl_AuthenticationUplink       = 9,
    MmElemUl_ExtendedCapabilities       = 11,
    MmElemUl_Proprietary                = 15
} MmType34ElemIdUl;

/* ---------------------------------------------------------------------------
 * GroupIdentityAttachment — 5-bit struct (§16.10.19).
 *   lifetime         2 bit
 *   class_of_usage   3 bit
 * Used in GroupIdentityDownlink when attach_detach_type_id=0 (attach).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t lifetime;       /* 0..3 — Gold-Ref M2 = 1 */
    uint8_t class_of_usage; /* 0..7 — Gold-Ref M2 = 4 */
} MmGroupIdentityAttachment;

/* ---------------------------------------------------------------------------
 * GroupIdentityDownlink — variable, §16.10.22.
 *   attach_detach_type_id  1 bit (0=attach, 1=detach)
 *   if attach (=0): group_identity_attachment (5 bits)
 *   if detach (=1): group_identity_detachment (2 bits)
 *   address_type           2 bits (0=GSSI, 1=+AE, 2=VGSSI, 3=GSSI+AE+VGSSI)
 *   gssi                   24 bits if address_type in {0,1,3}
 *   address_extension      24 bits if address_type in {1,3}
 *   vgssi                  24 bits if address_type in {2,3}
 * ------------------------------------------------------------------------- */
typedef struct {
    bool                       is_attach;            /* false = detach */
    MmGroupIdentityAttachment  attach;               /* used if is_attach */
    uint8_t                    detachment_type;      /* used if !is_attach */
    uint8_t                    address_type;         /* 0..3 */
    uint32_t                   gssi;                 /* low 24 bits, used if addr_type in {0,1,3} */
    uint32_t                   address_extension;    /* low 24 bits, used if addr_type in {1,3} */
    uint32_t                   vgssi;                /* low 24 bits, used if addr_type in {2,3} */
} MmGroupIdentityDownlink;

/* ---------------------------------------------------------------------------
 * GroupIdentityUplink — variable, §16.10.27.
 * Mirrors bluestation field but flattened into one struct with present flags.
 * Encoder selects address_type based on flags (gssi / ae / vgssi).
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     is_attach;           /* if true: class_of_usage encoded; else detachment */
    uint8_t  class_of_usage;      /* 0..7, if is_attach */
    uint8_t  detachment;          /* 0..3, if !is_attach */
    uint8_t  address_type;        /* 0..3 — 0=GSSI, 1=GSSI+AE, 2=VGSSI */
    uint32_t gssi;                /* 24 bit, if addr_type in {0,1} */
    uint32_t address_extension;   /* 24 bit, if addr_type == 1 */
    uint32_t vgssi;               /* 24 bit, if addr_type == 2 */
} MmGroupIdentityUplink;

/* ---------------------------------------------------------------------------
 * GroupIdentityLocationDemand (§16.10.24) — UL field embedded in
 * U-LOC-UPDATE-DEMAND. Carries one or more MS-requested GSSI demands.
 *
 * Layout:
 *   reserved                          1 bit (= 0)
 *   group_identity_attach_detach_mode 1 bit (0=amend, 1=replace-all)
 *   o-bit                             1 bit
 *   if o-bit:
 *     m-bit + elem_id(4) + length(11) + num_elems(6) + N × GIU + trailing m-bit
 * ------------------------------------------------------------------------- */
#define MM_GIU_MAX 4u

typedef struct {
    uint8_t                 attach_detach_mode;        /* 0=amend, 1=replace-all */
    uint8_t                 num_giu;                   /* 0..MM_GIU_MAX */
    MmGroupIdentityUplink   giu[MM_GIU_MAX];
} MmGild;  /* Group Identity Location Demand */

/* ---------------------------------------------------------------------------
 * MmDecoded — output of mm_iep_decode().
 * Structure can carry either a U-LOC-UPDATE-DEMAND or
 * U-ATTACH-DETACH-GROUP-IDENTITY (mm_pdu_type discriminates).
 * ------------------------------------------------------------------------- */
typedef struct {
    MmPduTypeUl        pdu_type;                /* 0..15 raw 4-bit value */

    /* U-LOC-UPDATE-DEMAND fields */
    LocationUpdateType location_update_type;
    bool               request_to_append_la;
    bool               cipher_control;
    bool               class_of_ms_present;
    uint32_t           class_of_ms;             /* 24 bit, low bits */
    bool               energy_saving_mode_present;
    EnergySavingMode   energy_saving_mode;
    bool               la_information_present;
    uint32_t           la_information;          /* 14 bit */
    bool               ssi_present;
    uint32_t           ssi;                     /* 24 bit */
    bool               address_extension_present;
    uint32_t           address_extension;       /* 24 bit */
    bool               gild_present;
    MmGild             gild;

    /* U-ATTACH-DETACH-GROUP-IDENTITY fields (mm_pdu_type=7) */
    bool               group_identity_report;
    uint8_t            attach_detach_mode;      /* 0=amend, 1=replace-all */
    uint8_t            num_giu;                 /* GIUs at top level for type=7 */
    MmGroupIdentityUplink giu[MM_GIU_MAX];
} MmDecoded;

/* ---------------------------------------------------------------------------
 * Builder parameter structs.
 * ------------------------------------------------------------------------- */
#define MM_GID_MAX 4u

typedef struct {
    LocationUpdateType        accept_type;             /* Gold M2: ItsiAttach */
    bool                      energy_saving_info_present;  /* Gold M2: true */
    EnergySavingMode          energy_saving_mode;          /* Gold M2: StayAlive */
    uint8_t                   energy_saving_frame_number;  /* 5 bit */
    uint8_t                   energy_saving_multiframe;    /* 6 bit */
    bool                      gila_present;            /* Gold M2: true */
    uint8_t                   gila_accept_reject;      /* 0=accept, 1=reject */
    uint8_t                   num_gid;                 /* 0..MM_GID_MAX */
    MmGroupIdentityDownlink   gid[MM_GID_MAX];
} MmAcceptParams;

typedef struct {
    uint8_t                   accept_reject;           /* 0=accept, 1=reject */
    bool                      gid_downlink_present;
    uint8_t                   num_gid;
    MmGroupIdentityDownlink   gid[MM_GID_MAX];
} MmGrpAckParams;

/* ---------------------------------------------------------------------------
 * IF_MM_v1 — public API.
 *
 * mm_iep_decode: parses a complete MM body (post-MAC, post-LLC, post-MLE-PD).
 *   Input bits start at the 4-bit MM pdu_type. Returns 0 on success,
 *   negative on parse error.
 *
 * mm_build_d_loc_update_accept: builds a D-LOC-UPDATE-ACCEPT MM body
 *   (starting at mm_pdu_type) per Gold-Ref M2 layout. Returns number of
 *   bits written, or -EINVAL/-ENOSPC on error.
 *
 * mm_build_d_attach_detach_grp_id_ack: builds a D-ATTACH-DETACH-GRP-ID-ACK
 *   MM body. Returns number of bits written, or negative on error.
 *
 * mm_init: zero-init the entity and register handlers on the bus.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t reserved;  /* placeholder for future cfg knobs */
} MmCfg;

typedef struct {
    size_t demand_decoded;
    size_t demand_decode_failed;
    size_t accept_built;
    size_t grp_ack_built;
} MmStats;

typedef struct {
    MsgBus       *bus;
    SubscriberDb *db;
    MmCfg         cfg;
    MmStats       stats;
    bool          initialised;
} Mm;

int mm_init(Mm *mm, MsgBus *bus, SubscriberDb *db, const MmCfg *cfg);

int mm_iep_decode(const uint8_t *bits, size_t len_bits, MmDecoded *out);

int mm_build_d_loc_update_accept(uint8_t *out, size_t cap_bits,
                                 const MmAcceptParams *p);

int mm_build_d_attach_detach_grp_id_ack(uint8_t *out, size_t cap_bits,
                                        const MmGrpAckParams *p);

/* ---------------------------------------------------------------------------
 * TlaSapPayload — MLE <-> MM message envelope (inter-layer SAP).
 *
 * MLE decodes the MM body via mm_iep_decode and posts a TlaSapPayload to
 * MM via TlmcSap; MM builds the accept/ack and (separately) signals
 * success back to MLE via TnmmSap. The same struct shape is used in
 * both directions; `is_attach` distinguishes "this is attach-side" vs
 * "this is detach-side" feedback for MLE FSM stepping.
 * ------------------------------------------------------------------------- */
typedef struct {
    EndpointId   endpoint;
    TetraAddress addr;
    MmDecoded    decoded;
    bool         is_attach;
} TlaSapPayload;

#ifdef __cplusplus
}
#endif

#endif /* TETRA_MM_H */

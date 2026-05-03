/* sw/mm/mm.c — MM (Mobility Management) entity + msgbus glue.
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MM_v1.
 *
 * MM is a dispatch shell: receives TlaSapPayload from MLE on the
 * (Mm, TlmcSap) tuple — those carry the already-decoded MM fields plus
 * the routing meta (endpoint + addr). MM consults the SubscriberDb for
 * Profile lookup (gila_class / gila_lifetime), composes
 * MmAcceptParams / MmGrpAckParams, calls the bit-exact builders, and
 * posts the resulting MM body upstream-to-LLC via TleSap on the bus.
 *
 * Cross-agent routing assumption (S3 contract): MLE-disc=1 PDUs (MM)
 * arrive at MM via TlmcSap from MLE; MLE-disc=5 PDUs (MLE-itself) never
 * reach MM (consumed in MLE for D-NWRK-BCAST etc.).
 */
#include "tetra/db.h"
#include "tetra/llc.h"
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * MLE-PD on-air encoding helper. The MM body produced by the builders
 * starts at the 4-bit mm_pdu_type. To hand it back to LLC we need to
 * prepend the 3-bit MLE-disc=001 (MM). We compose the result into a
 * caller buffer and pass body_len_bits = 3 + mm_bits.
 * ------------------------------------------------------------------------- */
static int prepend_mle_disc_mm(const uint8_t *mm_bits, size_t mm_len_bits,
                               uint8_t *out, size_t out_cap_bits,
                               size_t *out_len_bits)
{
    if (out == NULL || out_len_bits == NULL ||
        (mm_bits == NULL && mm_len_bits != 0u)) {
        return -EINVAL;
    }
    const size_t need = 3u + mm_len_bits;
    if (need > out_cap_bits) {
        return -ENOSPC;
    }

    memset(out, 0, (out_cap_bits + 7u) / 8u);
    /* Write MLE-disc = 001 (MM) at bit 0. */
    out[0] = (uint8_t) (0x1u << 5);
    /* Append mm_bits starting at bit 3. */
    for (size_t i = 0; i < mm_len_bits; ++i) {
        const size_t  src_byte = i >> 3;
        const uint8_t src_off  = (uint8_t) (7u - (i & 0x7u));
        const uint8_t bit      = (uint8_t) ((mm_bits[src_byte] >> src_off) & 0x1u);
        const size_t  dst_bit  = 3u + i;
        const size_t  dst_byte = dst_bit >> 3;
        const uint8_t dst_off  = (uint8_t) (7u - (dst_bit & 0x7u));
        const uint8_t mask     = (uint8_t) (1u << dst_off);
        if (bit) {
            out[dst_byte] |= mask;
        }
    }
    *out_len_bits = need;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Lookup Profile-driven defaults for a given ISSI.
 *
 * Returns true if a Profile was found (Entity + Profile valid). On miss,
 * leaves *gila_class / *gila_lifetime untouched. Per Gold-Ref M2 the
 * fallback values are 4 / 1 (Profile-0 invariant 0x0000088F).
 * ------------------------------------------------------------------------- */
static bool lookup_profile_defaults(SubscriberDb *db, uint32_t issi,
                                    uint8_t *gila_class,
                                    uint8_t *gila_lifetime,
                                    uint32_t *fallback_gssi_inout)
{
    if (db == NULL) {
        return false;
    }
    uint16_t idx = 0;
    if (db_lookup_entity(db, issi & TETRA_SSI_MASK_24, &idx) != 0) {
        return false;
    }
    Entity e;
    if (db_get_entity(db, idx, &e) != 0 || !e.valid) {
        return false;
    }
    Profile pr;
    if (db_get_profile(db, e.profile_id, &pr) != 0 || !pr.valid) {
        return false;
    }
    *gila_class    = pr.gila_class;
    *gila_lifetime = pr.gila_lifetime;
    (void) fallback_gssi_inout;  /* GSSI fallback comes from MleCfg, not Profile */
    return true;
}

/* ---------------------------------------------------------------------------
 * Compose D-LOC-UPDATE-ACCEPT MmAcceptParams from a decoded UL DEMAND
 * + DB-derived defaults. Mirrors Gold-Ref M2 layout.
 * ------------------------------------------------------------------------- */
static void compose_accept_params(const MmDecoded *demand,
                                  uint8_t gila_class,
                                  uint8_t gila_lifetime,
                                  uint32_t fallback_gssi,
                                  MmAcceptParams *out)
{
    memset(out, 0, sizeof(*out));
    out->accept_type = LocUpdate_ItsiAttach;

    /* Energy-saving-information: Gold-Ref M2 -> StayAlive, fn=0, mn=0. */
    out->energy_saving_info_present  = true;
    out->energy_saving_mode          = EnergySaving_StayAlive;
    out->energy_saving_frame_number  = 0;
    out->energy_saving_multiframe    = 0;

    /* GILA: Gold-Ref M2 -> attach, lifetime=1, class=4, gssi from demand
     * (or fallback when MS demanded none). */
    out->gila_present       = true;
    out->gila_accept_reject = 0;  /* accept */

    uint32_t gssi = fallback_gssi;
    /* If demand carried a GILD with at least one GIU GSSI, prefer the
     * first MS-demanded GSSI (Gold-Ref behaviour: BS echoes the MS's
     * requested GSSI in the GILA). */
    if (demand != NULL && demand->gild_present && demand->gild.num_giu > 0u &&
        (demand->gild.giu[0].address_type == 0u ||
         demand->gild.giu[0].address_type == 1u)) {
        gssi = demand->gild.giu[0].gssi & TETRA_SSI_MASK_24;
    }

    out->num_gid = 1;
    out->gid[0].is_attach           = true;
    out->gid[0].attach.lifetime     = gila_lifetime;
    out->gid[0].attach.class_of_usage = gila_class;
    out->gid[0].address_type        = 0;  /* GSSI only */
    out->gid[0].gssi                = gssi;
}

/* ---------------------------------------------------------------------------
 * msgbus handler — TlmcSap from MLE → MM (decoded MM PDU).
 *
 * Payload is a TlaSapPayload struct. MM dispatches by decoded.pdu_type.
 * ------------------------------------------------------------------------- */
static void on_mm_request(const SapMsg *msg, void *ctx)
{
    Mm *mm = (Mm *) ctx;
    if (mm == NULL || msg == NULL || msg->payload == NULL ||
        msg->len < sizeof(TlaSapPayload) || !mm->initialised) {
        return;
    }

    TlaSapPayload pl;
    memcpy(&pl, msg->payload, sizeof(pl));

    /* For UL-DEMAND types we build the corresponding accept and post it
     * back to LLC via TleSap (so LLC can wrap with NR/NS + MAC layer). */
    uint8_t  mm_body[64];   /* 512 bits — holds 102-bit M2 accept */
    int      mm_bits = -1;

    switch (pl.decoded.pdu_type) {
    case MmPduUl_ULocationUpdateDemand: {
        MmAcceptParams ap;
        uint8_t  gila_class    = 4;   /* Gold-Ref Profile-0 default */
        uint8_t  gila_lifetime = 1;
        uint32_t fallback_gssi = 0;
        (void) lookup_profile_defaults(mm->db, pl.addr.ssi,
                                       &gila_class, &gila_lifetime,
                                       &fallback_gssi);
        compose_accept_params(&pl.decoded, gila_class, gila_lifetime,
                              fallback_gssi, &ap);
        mm_bits = mm_build_d_loc_update_accept(mm_body, sizeof(mm_body) * 8u, &ap);
        if (mm_bits >= 0) {
            mm->stats.accept_built++;
        }
        break;
    }
    case MmPduUl_UAttachDetachGroupIdentity: {
        MmGrpAckParams gp;
        memset(&gp, 0, sizeof(gp));
        gp.accept_reject       = 0;
        gp.gid_downlink_present = (pl.decoded.num_giu > 0u);
        gp.num_gid             = 0;
        for (uint8_t i = 0; i < pl.decoded.num_giu && gp.num_gid < MM_GID_MAX; ++i) {
            const MmGroupIdentityUplink *src = &pl.decoded.giu[i];
            MmGroupIdentityDownlink     *dst = &gp.gid[gp.num_gid++];
            dst->is_attach = src->is_attach;
            if (src->is_attach) {
                /* Gold-Ref: lifetime=1, class_of_usage matches MS request. */
                dst->attach.lifetime       = 1;
                dst->attach.class_of_usage = src->class_of_usage;
            } else {
                dst->detachment_type = src->detachment;
            }
            dst->address_type      = src->address_type;
            dst->gssi              = src->gssi;
            dst->address_extension = src->address_extension;
            dst->vgssi             = src->vgssi;
        }
        mm_bits = mm_build_d_attach_detach_grp_id_ack(mm_body,
                                                     sizeof(mm_body) * 8u, &gp);
        if (mm_bits >= 0) {
            mm->stats.grp_ack_built++;
        }
        break;
    }
    default:
        return;
    }

    if (mm_bits < 0) {
        return;
    }

    /* Wrap with MLE-disc=001 to form the LLC body and post via TleSap. */
    uint8_t  llc_body[LLC_PDU_BODY_MAX_BYTES];
    size_t   llc_body_bits = 0;
    if (prepend_mle_disc_mm(mm_body, (size_t) mm_bits,
                            llc_body, sizeof(llc_body) * 8u,
                            &llc_body_bits) != 0) {
        return;
    }

    TleSapMsg out;
    memset(&out, 0, sizeof(out));
    out.endpoint           = pl.endpoint;
    out.addr               = pl.addr;
    out.pdu.pdu_type       = LlcPdu_BL_ADATA;
    out.pdu.body_len_bits  = (uint16_t) llc_body_bits;
    if (llc_body_bits > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return;
    }
    memcpy(out.pdu.body, llc_body, (llc_body_bits + 7u) / 8u);

    SapMsg env;
    sapmsg_init(&env, SapId_TmaSap /* MM-as-source-from-LLC POV */,
                SapId_TmaSap, SapId_TleSap,
                (const uint8_t *) &out, (uint16_t) sizeof(out));
    (void) msgbus_post(mm->bus, sap_prio_default(SapId_TleSap), &env);
}

/* ---------------------------------------------------------------------------
 * mm_init — register handlers on the bus.
 * ------------------------------------------------------------------------- */
int mm_init(Mm *mm, MsgBus *bus, SubscriberDb *db, const MmCfg *cfg)
{
    if (mm == NULL || bus == NULL) {
        return -EINVAL;
    }
    memset(mm, 0, sizeof(*mm));
    mm->bus = bus;
    mm->db  = db;
    if (cfg != NULL) {
        mm->cfg = *cfg;
    }

    /* MM listens for "MM request" envelopes posted by MLE (after MLE
     * stripped MLE-PD and decoded the MM body). The (dest, sap) tuple
     * we register on is (TmaSap=consumer-side dummy, TlmcSap). */
    int rc = msgbus_register(bus, SapId_TmaSap, SapId_TlmcSap,
                             on_mm_request, mm);
    if (rc != 0) {
        return rc;
    }

    mm->initialised = true;
    return 0;
}

/* sw/mm/mm_accept_builder.c — MM Accept-PDU builders (DL encode).
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MM_v1.
 *
 * Bit-exact builders for:
 *   - D-LOCATION-UPDATE-ACCEPT (MmPduDl=5), 102-bit MM body per
 *     docs/references/reference_gold_attach_bitexact.md Z.111-152
 *   - D-ATTACH-DETACH-GROUP-IDENTITY-ACK (MmPduDl=11), variable MM body
 *     per docs/references/reference_group_attach_bitexact.md
 *
 * Source-of-truth (CLAUDE.md §1): Gold > Bluestation > ETSI.
 *
 * Type-2: write 1 p-bit; if Some, also write data bits.
 * Type-3: nothing if None; if Some, write m-bit(=1)+id(4)+length(11)+payload.
 * Type-4: nothing if None; if Some, write m-bit(=1)+id(4)+length(11)+
 *         num_elems(6)+payload.
 * Trailing m-bit: only emitted if obit was 1.
 */
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/types.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Local write-cursor wrapping an output byte buffer in MSB-first bit order.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *bits;
    size_t   cap_bits;
    size_t   pos;
    bool     overflow;
} WBuf;

static void wbuf_put(WBuf *w, uint64_t v, uint8_t n)
{
    if (n == 0u) return;
    if (n > 64u || w->pos + n > w->cap_bits) {
        w->overflow = true;
        return;
    }
    for (uint8_t i = 0; i < n; ++i) {
        const uint64_t bit = (v >> (n - 1u - i)) & 0x1u;
        const size_t   bit_idx = w->pos + i;
        const size_t   byte_idx = bit_idx >> 3;
        const uint8_t  bit_off  = (uint8_t) (7u - (bit_idx & 0x7u));
        const uint8_t  mask     = (uint8_t) (1u << bit_off);
        if (bit) {
            w->bits[byte_idx] |= mask;
        } else {
            w->bits[byte_idx] = (uint8_t) (w->bits[byte_idx] & (uint8_t) ~mask);
        }
    }
    w->pos += n;
}

/* Patch a length field (11 bits) at an absolute bit position. Used to
 * back-fill Type-3/Type-4 length after the payload is written. */
static void wbuf_patch_11(WBuf *w, size_t pos, uint16_t v11)
{
    if (pos + 11u > w->cap_bits) {
        w->overflow = true;
        return;
    }
    for (uint8_t i = 0; i < 11u; ++i) {
        const uint16_t bit = (uint16_t) ((v11 >> (10u - i)) & 0x1u);
        const size_t   bit_idx = pos + i;
        const size_t   byte_idx = bit_idx >> 3;
        const uint8_t  bit_off  = (uint8_t) (7u - (bit_idx & 0x7u));
        const uint8_t  mask     = (uint8_t) (1u << bit_off);
        if (bit) {
            w->bits[byte_idx] |= mask;
        } else {
            w->bits[byte_idx] = (uint8_t) (w->bits[byte_idx] & (uint8_t) ~mask);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Type-2 generic — emit 1-bit p-bit, plus data when present.
 * ------------------------------------------------------------------------- */
static void put_type2_generic(WBuf *w, bool present,
                              uint64_t value, uint8_t width)
{
    wbuf_put(w, present ? 1u : 0u, 1);
    if (present) {
        wbuf_put(w, value, width);
    }
}

/* ---------------------------------------------------------------------------
 * Encode GroupIdentityDownlink struct (variable, §16.10.22).
 * ------------------------------------------------------------------------- */
static void put_gid_downlink(WBuf *w, const MmGroupIdentityDownlink *g)
{
    wbuf_put(w, g->is_attach ? 0u : 1u, 1);
    if (g->is_attach) {
        wbuf_put(w, (uint64_t) g->attach.lifetime,       2);
        wbuf_put(w, (uint64_t) g->attach.class_of_usage, 3);
    } else {
        wbuf_put(w, (uint64_t) g->detachment_type, 2);
    }
    wbuf_put(w, (uint64_t) g->address_type, 2);
    if (g->address_type == 0u || g->address_type == 1u || g->address_type == 3u) {
        wbuf_put(w, (uint64_t) (g->gssi & TETRA_SSI_MASK_24), 24);
    }
    if (g->address_type == 1u || g->address_type == 3u) {
        wbuf_put(w, (uint64_t) (g->address_extension & TETRA_SSI_MASK_24), 24);
    }
    if (g->address_type == 2u || g->address_type == 3u) {
        wbuf_put(w, (uint64_t) (g->vgssi & TETRA_SSI_MASK_24), 24);
    }
}

/* Encode the GID-Downlink Type-4 list with header + back-filled length.
 * No-op if num_gid == 0 (Type-4 absent → no m-bit). */
static void put_gid_downlink_list(WBuf *w,
                                  uint8_t num_gid,
                                  const MmGroupIdentityDownlink *gids)
{
    if (num_gid == 0u) {
        return;
    }
    wbuf_put(w, 1u, 1);                              /* m-bit */
    wbuf_put(w, (uint64_t) MmElemDl_GroupIdentityDownlink, 4); /* elem_id = 7 */
    const size_t length_pos = w->pos;
    wbuf_put(w, 0u, 11);                             /* length placeholder */
    const size_t after_length = w->pos;
    wbuf_put(w, (uint64_t) num_gid, 6);              /* num_elems */
    for (uint8_t i = 0; i < num_gid; ++i) {
        put_gid_downlink(w, &gids[i]);
    }
    /* Length covers num_elems(6) + payload bits, i.e. all bits written
     * after the 11-bit length field. */
    const size_t actual_after_lf = w->pos - after_length;
    const uint16_t length = (uint16_t) actual_after_lf;
    wbuf_patch_11(w, length_pos, length);
}

/* ---------------------------------------------------------------------------
 * Encode GroupIdentityLocationAccept Type-3 element (§16.10.23).
 *
 * Payload (variable):
 *   group_identity_accept_reject  1 bit
 *   reserved                      1 bit (= 0)
 *   o-bit                         1 bit
 *   if obit: GID-Downlink list (Type-4) + trailing m-bit
 *
 * Wrapper writes m-bit=1, elem_id=5, length=payload-bits, then payload.
 * ------------------------------------------------------------------------- */
static void put_gila(WBuf *w, const MmAcceptParams *p)
{
    wbuf_put(w, 1u, 1);                                  /* m-bit */
    wbuf_put(w, (uint64_t) MmElemDl_GroupIdentityLocationAccept, 4); /* elem_id=5 */
    const size_t length_pos = w->pos;
    wbuf_put(w, 0u, 11);                                 /* length placeholder */
    const size_t payload_start = w->pos;

    /* GILA payload */
    wbuf_put(w, (uint64_t) p->gila_accept_reject, 1);
    wbuf_put(w, 0u, 1);                                  /* reserved */
    const bool inner_obit = (p->num_gid > 0u);
    wbuf_put(w, inner_obit ? 1u : 0u, 1);
    if (inner_obit) {
        put_gid_downlink_list(w, p->num_gid, p->gid);
        wbuf_put(w, 0u, 1);                              /* trailing m-bit */
    }

    const uint16_t length = (uint16_t) (w->pos - payload_start);
    wbuf_patch_11(w, length_pos, length);
}

/* ---------------------------------------------------------------------------
 * mm_build_d_loc_update_accept — D-LOC-UPDATE-ACCEPT MM body builder.
 *
 * Layout per Gold-Ref reference_gold_attach_bitexact.md Z.111-152 (102 bits):
 *   [0..3]   mm_pdu_type        = 0101 (=5, DLocationUpdateAccept)
 *   [4..6]   loc_acc_type       = 011 (=3, ItsiAttach)
 *   [7]      o-bit              = 1
 *   [8]      p_ssi              = 0 (Gold-Ref)
 *   [9]      p_ae               = 0
 *   [10]     p_subscriber_class = 0
 *   [11]     p_energy_saving    = 1
 *   [12..25] energy_saving_info = 14×0 (StayAlive)
 *   [26]     p_scch             = 0
 *   GILA Type-3 element (74 bits Gold)
 *   trailing m-bit = 0
 *
 * Returns number of bits written. -EINVAL/-ENOSPC on error.
 * ------------------------------------------------------------------------- */
int mm_build_d_loc_update_accept(uint8_t *out, size_t cap_bits,
                                 const MmAcceptParams *p)
{
    if (out == NULL || p == NULL || cap_bits < 16u) {
        return -EINVAL;
    }
    /* Zero output so unused tail bits stay 0. */
    memset(out, 0, (cap_bits + 7u) / 8u);

    WBuf w = { .bits = out, .cap_bits = cap_bits, .pos = 0, .overflow = false };

    /* Header. */
    wbuf_put(&w, (uint64_t) MmPduDl_DLocationUpdateAccept, 4);
    wbuf_put(&w, (uint64_t) (p->accept_type & 0x7u), 3);

    /* Determine o-bit. Gold-Ref has GILA + ESI present → obit=1.
     * In general: obit=1 iff any optional field is present. */
    const bool obit = p->energy_saving_info_present || p->gila_present;
    wbuf_put(&w, obit ? 1u : 0u, 1);

    if (!obit) {
        return w.overflow ? -ENOSPC : (int) w.pos;
    }

    /* Type-2 fields in order from bluestation `to_bitbuf`. Gold-Ref M2
     * has all None except energy_saving_information. */
    put_type2_generic(&w, false, 0u, 24);  /* p_ssi               (0) */
    put_type2_generic(&w, false, 0u, 24);  /* p_address_extension (0) */
    put_type2_generic(&w, false, 0u, 16);  /* p_subscriber_class  (0) */

    /* Type-2 struct: energy_saving_information (3+5+6 = 14 bits). */
    if (p->energy_saving_info_present) {
        wbuf_put(&w, 1u, 1);  /* p-bit = 1 */
        wbuf_put(&w, (uint64_t) (p->energy_saving_mode & 0x7u),    3);
        wbuf_put(&w, (uint64_t) (p->energy_saving_frame_number & 0x1Fu), 5);
        wbuf_put(&w, (uint64_t) (p->energy_saving_multiframe & 0x3Fu),   6);
    } else {
        wbuf_put(&w, 0u, 1);  /* p-bit = 0 */
    }

    put_type2_generic(&w, false, 0u, 6);   /* p_scch_info_distrib (0) */

    /* Type-4: new_registered_area — None ⇒ no bits. */
    /* Type-3: security_downlink — None ⇒ no bits. */

    /* Type-3 struct: group_identity_location_accept. */
    if (p->gila_present) {
        put_gila(&w, p);
    }

    /* Type-3: default_group_attachment_lifetime — None. */
    /* Type-3: authentication_downlink — None. */
    /* Type-4: group_identity_security_related_information — None. */
    /* Type-3: cell_type_control — None. */
    /* Type-3: proprietary — None. */

    /* Trailing m-bit (= 0) since obit was 1. */
    wbuf_put(&w, 0u, 1);

    if (w.overflow) {
        return -ENOSPC;
    }
    return (int) w.pos;
}

/* ---------------------------------------------------------------------------
 * mm_build_d_attach_detach_grp_id_ack — D-ATTACH-DETACH-GRP-ID-ACK builder.
 *
 * Layout per Gold-Ref reference_group_attach_bitexact.md (62-bit MM body
 * for the typical Gold case num_gid=1, GSSI-only):
 *   [0..3]  mm_pdu_type           = 1011 (=11)
 *   [4]     accept_reject         (= 0 for ACCEPT, 1 for REJECT)
 *   [5]     reserved              = 0
 *   [6]     o-bit                 = 1 (GID-Downlink list present)
 *     Type-3 proprietary: None (no bits)
 *     Type-4 group_identity_downlink: list
 *     Type-4 group_identity_security: None
 *   trailing m-bit = 0
 * ------------------------------------------------------------------------- */
int mm_build_d_attach_detach_grp_id_ack(uint8_t *out, size_t cap_bits,
                                        const MmGrpAckParams *p)
{
    if (out == NULL || p == NULL || cap_bits < 8u) {
        return -EINVAL;
    }
    memset(out, 0, (cap_bits + 7u) / 8u);

    WBuf w = { .bits = out, .cap_bits = cap_bits, .pos = 0, .overflow = false };

    wbuf_put(&w, (uint64_t) MmPduDl_DAttachDetachGroupIdentityAck, 4);
    wbuf_put(&w, (uint64_t) (p->accept_reject & 0x1u), 1);
    wbuf_put(&w, 0u, 1);  /* reserved */

    const bool obit = p->gid_downlink_present && (p->num_gid > 0u);
    wbuf_put(&w, obit ? 1u : 0u, 1);
    if (!obit) {
        return w.overflow ? -ENOSPC : (int) w.pos;
    }

    /* Type-3 proprietary: None ⇒ no bits. */
    /* Type-4 group_identity_downlink list. */
    put_gid_downlink_list(&w, p->num_gid, p->gid);
    /* Type-4 group_identity_security: None ⇒ no bits. */

    /* Trailing m-bit. */
    wbuf_put(&w, 0u, 1);

    if (w.overflow) {
        return -ENOSPC;
    }
    return (int) w.pos;
}

/* sw/mm/mm_iep.c — MM IE-Parser (UL decode).
 *
 * Owned by S3 (S3-sw-mle-mm). Locked under interface contract IF_MM_v1.
 *
 * Parses U-LOCATION-UPDATE-DEMAND (mm_pdu_type=2) and
 * U-ATTACH-DETACH-GROUP-IDENTITY (mm_pdu_type=7) MM bodies from a
 * 129-bit reassembled UL bit-buffer (per
 * docs/references/reference_demand_reassembly_bitexact.md).
 *
 * Type-2 fields use p-bits (only 1 bit when None, p+data when Some).
 * Type-3/Type-4 fields use m-bits (0 bits when None, m+id+length+payload
 * when Some). Trailing m-bit terminates the optional-list. Mirrors
 * bluestation `tetra-pdus/src/mm/pdus/{u_location_update_demand,
 * u_attach_detach_group_identity}.rs`.
 *
 * Source-of-truth: gold_field_values.md "U-LOCATION-UPDATE-DEMAND" rows.
 */
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/types.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Local cursor wrapping a const byte buffer (read-only BitBuffer-like).
 * ------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *bits;
    size_t         len_bits;
    size_t         pos;
    bool           overrun;
} RBuf;

static uint32_t rbuf_read(RBuf *r, uint8_t n)
{
    if (n == 0u || n > 32u) {
        return 0u;
    }
    if (r->pos + n > r->len_bits) {
        r->overrun = true;
        return 0u;
    }
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; ++i) {
        const size_t  bit_idx = r->pos + i;
        const size_t  byte_idx = bit_idx >> 3;
        const uint8_t bit_off = (uint8_t) (7u - (bit_idx & 0x7u));
        const uint32_t b = (uint32_t) ((r->bits[byte_idx] >> bit_off) & 0x1u);
        v = (v << 1) | b;
    }
    r->pos += n;
    return v;
}

static bool rbuf_read_bit(RBuf *r) { return rbuf_read(r, 1) != 0u; }

/* ---------------------------------------------------------------------------
 * Type-2 helpers — read p-bit, then `width` bits if present.
 * Returns true if field present, false if absent.
 * ------------------------------------------------------------------------- */
static bool decode_type2_generic(RBuf *r, bool obit, uint8_t width,
                                 uint32_t *out_value)
{
    if (!obit) {
        *out_value = 0u;
        return false;
    }
    const bool present = rbuf_read_bit(r);
    if (!present) {
        *out_value = 0u;
        return false;
    }
    *out_value = rbuf_read(r, width);
    return true;
}

/* Like decode_type2_generic but the field is wider than 32 bits.
 * Reads `width` bits via two pulls if needed. */
static bool decode_type2_u64(RBuf *r, bool obit, uint8_t width,
                             uint64_t *out_value)
{
    if (!obit) {
        *out_value = 0u;
        return false;
    }
    const bool present = rbuf_read_bit(r);
    if (!present) {
        *out_value = 0u;
        return false;
    }
    uint64_t v = 0;
    while (width > 32u) {
        v = (v << 32) | (uint64_t) rbuf_read(r, 32);
        width = (uint8_t) (width - 32u);
    }
    if (width > 0u) {
        v = (v << width) | (uint64_t) rbuf_read(r, width);
    }
    *out_value = v;
    return true;
}

/* ---------------------------------------------------------------------------
 * Type-3/4 element header — peek m-bit + 4-bit elem_id without advancing.
 * Bluestation `peek_type34_mbit_and_id` semantics: returns true if
 * (mbit==1 && id==expected). Does NOT advance cursor; caller advances by
 * 5 bits when consuming.
 * ------------------------------------------------------------------------- */
static bool peek_type34(const RBuf *r, uint8_t expected_id)
{
    if (r->pos + 5u > r->len_bits) {
        return false;
    }
    /* Save and re-read 5 bits without advancing. */
    RBuf tmp = *r;
    const uint32_t mbit = rbuf_read(&tmp, 1);
    if (mbit == 0u) {
        return false;
    }
    const uint32_t id = rbuf_read(&tmp, 4);
    return id == (uint32_t) expected_id;
}

/* Advance past the 5-bit Type-3/4 header after a successful peek. */
static void skip_type34_header(RBuf *r)
{
    (void) rbuf_read(r, 5);
}

/* ---------------------------------------------------------------------------
 * Decode a single GroupIdentityUplink struct (variable, §16.10.27).
 * Returns 0 on success; -EPROTO on parse error.
 * ------------------------------------------------------------------------- */
static int decode_giu(RBuf *r, MmGroupIdentityUplink *out)
{
    memset(out, 0, sizeof(*out));
    const uint32_t adt = rbuf_read(r, 1);
    out->is_attach = (adt == 0u);
    if (out->is_attach) {
        out->class_of_usage = (uint8_t) rbuf_read(r, 3);
    } else {
        out->detachment = (uint8_t) rbuf_read(r, 2);
    }
    out->address_type = (uint8_t) rbuf_read(r, 2);
    if (out->address_type == 0u || out->address_type == 1u) {
        out->gssi = rbuf_read(r, 24) & TETRA_SSI_MASK_24;
    }
    if (out->address_type == 1u) {
        out->address_extension = rbuf_read(r, 24) & TETRA_SSI_MASK_24;
    }
    if (out->address_type == 2u) {
        out->vgssi = rbuf_read(r, 24) & TETRA_SSI_MASK_24;
    }
    return r->overrun ? -EPROTO : 0;
}

/* ---------------------------------------------------------------------------
 * Decode the Group-Identity-Uplink Type-4 wrapper (header + N×GIU).
 *
 * On entry: cursor positioned at the m-bit of the GroupIdentityUplink
 * Type-4 element. On success, advances past the entire element and writes
 * up to MM_GIU_MAX entries to `giu_out`.
 * ------------------------------------------------------------------------- */
static int decode_giu_list(RBuf *r,
                           uint8_t *out_count,
                           MmGroupIdentityUplink *giu_out)
{
    *out_count = 0;
    if (!peek_type34(r, (uint8_t) MmElemUl_GroupIdentityUplink)) {
        return 0;
    }
    skip_type34_header(r);
    const uint16_t length    = (uint16_t) rbuf_read(r, 11);
    const uint8_t  num_elems = (uint8_t) rbuf_read(r, 6);
    const size_t   start_pos = r->pos;
    (void) length; /* sanity-checked by reading num_elems exactly */

    const uint8_t cap = (uint8_t) MM_GIU_MAX;
    for (uint8_t i = 0; i < num_elems; ++i) {
        if (i < cap) {
            int rc = decode_giu(r, &giu_out[i]);
            if (rc != 0) {
                return rc;
            }
        } else {
            /* Beyond cap: skip raw bits — but we don't know bit-width
             * without parsing. Bail with EPROTO. */
            return -EPROTO;
        }
    }
    *out_count = (num_elems < cap) ? num_elems : cap;

    /* Sanity: payload bit length must equal length-6 (length includes
     * num_elems(6)). Bluestation: pos_end - pos_len_field - 11 ==
     * actual bits, where actual = 6 (num_elems) + struct_bits. */
    if (length >= 6u && (r->pos - start_pos) != (size_t) (length - 6u)) {
        return -EPROTO;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Decode GroupIdentityLocationDemand (16.10.24).
 * ------------------------------------------------------------------------- */
static int decode_gild(RBuf *r, MmGild *out)
{
    memset(out, 0, sizeof(*out));
    /* reserved (1) + attach_detach_mode (1) */
    const uint32_t reserved = rbuf_read(r, 1);
    if (reserved != 0u) {
        return -EPROTO;
    }
    out->attach_detach_mode = (uint8_t) rbuf_read(r, 1);

    const bool obit = rbuf_read_bit(r);
    if (obit) {
        int rc = decode_giu_list(r, &out->num_giu, out->giu);
        if (rc != 0) {
            return rc;
        }
        /* trailing m-bit (= 0). Bluestation reads conditionally on obit. */
        const uint32_t trailing = rbuf_read(r, 1);
        if (trailing != 0u) {
            return -EPROTO;
        }
    }
    return r->overrun ? -EPROTO : 0;
}

/* ---------------------------------------------------------------------------
 * Decode Type-3 generic — skip an unknown-but-present m-bit-tagged
 * element. Used to silently consume optional fields we don't expose.
 * Returns 0 on success.
 * ------------------------------------------------------------------------- */
static int decode_type3_generic_skip(RBuf *r, uint8_t expected_id)
{
    if (!peek_type34(r, expected_id)) {
        return 0;
    }
    skip_type34_header(r);
    const uint16_t length = (uint16_t) rbuf_read(r, 11);
    if (length > 256u) {
        return -EPROTO;
    }
    /* Advance past `length` data bits. */
    if (r->pos + length > r->len_bits) {
        r->overrun = true;
        return -EPROTO;
    }
    r->pos += length;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Decode U-LOCATION-UPDATE-DEMAND.
 * Cursor is on the 4-bit pdu_type field at entry.
 * ------------------------------------------------------------------------- */
static int decode_u_loc_update_demand(RBuf *r, MmDecoded *out)
{
    const uint32_t pdu_type = rbuf_read(r, 4);
    if (pdu_type != (uint32_t) MmPduUl_ULocationUpdateDemand) {
        return -EPROTO;
    }
    out->pdu_type = MmPduUl_ULocationUpdateDemand;

    out->location_update_type = (LocationUpdateType) rbuf_read(r, 3);
    out->request_to_append_la = rbuf_read_bit(r);
    out->cipher_control       = rbuf_read_bit(r);
    if (out->cipher_control) {
        /* ciphering_parameters: 10 bits. We discard them but consume the bits. */
        (void) rbuf_read(r, 10);
    }

    const bool obit = rbuf_read_bit(r);
    if (!obit) {
        return r->overrun ? -EPROTO : 0;
    }

    /* Type-2: class_of_ms (24-bit struct, treated as opaque 24-bit value). */
    {
        uint64_t v = 0;
        out->class_of_ms_present = decode_type2_u64(r, obit, 24, &v);
        out->class_of_ms = (uint32_t) v;
    }
    /* Type-2: energy_saving_mode (3 bits). */
    {
        uint32_t v = 0;
        out->energy_saving_mode_present = decode_type2_generic(r, obit, 3, &v);
        out->energy_saving_mode = (EnergySavingMode) v;
    }
    /* Type-2: la_information (15 bits = 14 LA + 1 trailing zero). */
    {
        uint32_t v = 0;
        out->la_information_present = decode_type2_generic(r, obit, 15, &v);
        out->la_information = v >> 1;
    }
    /* Type-2: ssi (24 bits). */
    {
        uint32_t v = 0;
        out->ssi_present = decode_type2_generic(r, obit, 24, &v);
        out->ssi = v & TETRA_SSI_MASK_24;
    }
    /* Type-2: address_extension (24 bits). */
    {
        uint32_t v = 0;
        out->address_extension_present = decode_type2_generic(r, obit, 24, &v);
        out->address_extension = v & TETRA_SSI_MASK_24;
    }

    /* Type-3: GroupIdentityLocationDemand. */
    if (peek_type34(r, (uint8_t) MmElemUl_GroupIdentityLocationDemand)) {
        skip_type34_header(r);
        const uint16_t length    = (uint16_t) rbuf_read(r, 11);
        const size_t   start_pos = r->pos;
        out->gild_present = true;
        int rc = decode_gild(r, &out->gild);
        if (rc != 0) {
            return rc;
        }
        if ((r->pos - start_pos) != length) {
            return -EPROTO;
        }
    }

    /* Skip remaining optional Type-3 fields. */
    int rc;
    rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_GroupReportResponse);
    if (rc != 0) return rc;
    rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_AuthenticationUplink);
    if (rc != 0) return rc;
    rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_ExtendedCapabilities);
    if (rc != 0) return rc;
    rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_Proprietary);
    if (rc != 0) return rc;

    /* Trailing m-bit (must be 0). */
    if (r->pos < r->len_bits) {
        const uint32_t trailing = rbuf_read(r, 1);
        if (trailing != 0u) {
            return -EPROTO;
        }
    }

    return r->overrun ? -EPROTO : 0;
}

/* ---------------------------------------------------------------------------
 * Decode U-ATTACH-DETACH-GROUP-IDENTITY (mm_pdu_type=7).
 *
 * Per reference_group_attach_bitexact.md UL-Demand MM-Body Layout:
 *   [0..3] pdu_type=0111
 *   [4]    group_identity_report (1 bit)
 *   [5]    attach_detach_mode    (1 bit)
 *   [6]    o-bit                 (1 bit)
 *     if obit:
 *       Type-3 group_report_response (1 mbit, optional)
 *       Type-4 group_identity_uplink (1 mbit, list)
 *       Type-3 proprietary           (1 mbit)
 *   trailing m-bit = 0
 * ------------------------------------------------------------------------- */
static int decode_u_attach_detach_grp_id(RBuf *r, MmDecoded *out)
{
    const uint32_t pdu_type = rbuf_read(r, 4);
    if (pdu_type != (uint32_t) MmPduUl_UAttachDetachGroupIdentity) {
        return -EPROTO;
    }
    out->pdu_type = MmPduUl_UAttachDetachGroupIdentity;

    out->group_identity_report = rbuf_read_bit(r);
    out->attach_detach_mode    = (uint8_t) rbuf_read(r, 1);

    const bool obit = rbuf_read_bit(r);
    if (!obit) {
        return r->overrun ? -EPROTO : 0;
    }

    /* Type-3: GroupReportResponse (skip). */
    int rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_GroupReportResponse);
    if (rc != 0) return rc;

    /* Type-4: GroupIdentityUplink list. */
    rc = decode_giu_list(r, &out->num_giu, out->giu);
    if (rc != 0) return rc;

    /* Type-3: Proprietary (skip). */
    rc = decode_type3_generic_skip(r, (uint8_t) MmElemUl_Proprietary);
    if (rc != 0) return rc;

    /* Trailing m-bit. */
    if (r->pos < r->len_bits) {
        const uint32_t trailing = rbuf_read(r, 1);
        if (trailing != 0u) {
            return -EPROTO;
        }
    }
    return r->overrun ? -EPROTO : 0;
}

/* ---------------------------------------------------------------------------
 * mm_iep_decode — public dispatcher.
 * Inspects mm_pdu_type at bit 0 and dispatches.
 * ------------------------------------------------------------------------- */
int mm_iep_decode(const uint8_t *bits, size_t len_bits, MmDecoded *out)
{
    if (bits == NULL || out == NULL || len_bits < 4u) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));

    /* Peek pdu_type without consuming. */
    RBuf peek = { .bits = bits, .len_bits = len_bits, .pos = 0, .overrun = false };
    const uint32_t pdu_type = rbuf_read(&peek, 4);
    if (peek.overrun) {
        return -EPROTO;
    }

    RBuf r = { .bits = bits, .len_bits = len_bits, .pos = 0, .overrun = false };
    switch (pdu_type) {
    case MmPduUl_ULocationUpdateDemand:
        return decode_u_loc_update_demand(&r, out);
    case MmPduUl_UAttachDetachGroupIdentity:
        return decode_u_attach_detach_grp_id(&r, out);
    default:
        out->pdu_type = (MmPduTypeUl) pdu_type;
        return -ENOTSUP;
    }
}

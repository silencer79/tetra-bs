/* sw/llc/llc_pdu.c — LLC PDU encode/decode (bit-exact on-air) + CRC-32.
 *
 * Owned by S2 (S2-sw-llc). Locked under interface contract IF_LLC_v1.
 *
 * Implements:
 *   - llc_pdu_encode / llc_pdu_decode for every LlcPduType in llc.h
 *     (BL-DATA, BL-ADATA, BL-UDATA, BL-ACK, AL-SETUP, plus +FCS variants)
 *   - llc_crc32 — CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320,
 *     input/output, init/xorout = 0xFFFFFFFF) over MSB-first bit-buffer
 *
 * Bit-layout per source-of-truth (CLAUDE.md §1):
 *   Gold-Ref: docs/references/reference_gold_attach_bitexact.md
 *     UL#0 BL-DATA  : pdu_type[4] | NS[1] | body
 *     UL#2 BL-ACK   : pdu_type[4] | NR[1] | body
 *     DL#735 BL-ADATA: pdu_type[4] | NR[1] | NS[1] | body
 *     DL#727 AL-SETUP: pdu_type[4] | (no NR/NS, no body — wrapper only)
 *   Bluestation: llc/pdu_type.rs — same field order, +FCS variants append
 *     a 32-bit FCS after `body`.
 *   ETSI EN 300 392-2 §22 — tie-break only (CRC-32 polynomial); the §22
 *     PDF is not in docs/references/, see llc.h:llc_crc32 TODO.
 *
 * MSB-first throughout, matching BitBuffer semantics from sw/core/.
 */
#include "tetra/llc.h"
#include "tetra/msgbus.h"
#include "tetra/types.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * CRC-32 — table-free, bit-serial. Carried over from `tetra-zynq-phy`
 * production decoder (`/home/kevin/claude-ralph/tetra/scripts/decode_dl.py
 * :crc32_check_llc`) which has been validated against on-air captures.
 *
 *   poly = 0xEDB88320   (reflected IEEE 802.3 generator)
 *   init = 0xFFFFFFFF
 *   xor  = 0xFFFFFFFF   (encoder side; receiver uses residue check instead)
 *   bit-feed: per-bit, MSB-first within each byte (matches BitBuffer +
 *             on-air TETRA bit order). Each bit XORs into the CRC register
 *             LSB, then crc shifts right; reflected polynomial XORs back
 *             on feedback=1.
 *
 * Receiver residue check: feed data + appended 32-bit FCS bits in the
 * same order; residue equals 0xDEBB20E3 ⇒ FCS valid. Mirrors
 * tetra-zynq-phy `crc32_check_llc` exactly.
 * ------------------------------------------------------------------------- */
#define LLC_CRC32_POLY    0xEDB88320u
#define LLC_CRC32_INIT    0xFFFFFFFFu
#define LLC_CRC32_XOROUT  0xFFFFFFFFu
#define LLC_CRC32_RESIDUE 0xDEBB20E3u

uint32_t llc_crc32(const uint8_t *data, size_t len_bits)
{
    if (data == NULL || len_bits == 0u) {
        return LLC_CRC32_INIT ^ LLC_CRC32_XOROUT;
    }

    uint32_t crc = LLC_CRC32_INIT;

    for (size_t i = 0; i < len_bits; ++i) {
        const size_t  byte_idx = i >> 3;
        const uint8_t bit_off  = (uint8_t) (7u - (i & 0x7u));
        const uint32_t bit     = (uint32_t) ((data[byte_idx] >> bit_off) & 0x1u);

        /* Reflected form (carry-over from tetra-zynq-phy decode_dl.py): each
         * input bit XORs into LSB; if feedback=1, crc ^= reflected-poly. */
        const uint32_t feedback = (bit ^ (crc & 0x1u)) & 0x1u;
        crc >>= 1;
        if (feedback) {
            crc ^= LLC_CRC32_POLY;
        }
    }

    return crc ^ LLC_CRC32_XOROUT;
}

/* ---------------------------------------------------------------------------
 * Internal helpers.
 * ------------------------------------------------------------------------- */

/* Compute CRC-32 over a window of the encoded PDU. We re-read MSB-first
 * from the BitBuffer's underlying bytes between abs-positions [start..end).
 * Caller guarantees byte-aligned start (always true: pdu_type is the very
 * first field at bb start). */
static uint32_t crc32_over_bb_window(const BitBuffer *bb,
                                     size_t start_bit, size_t end_bit)
{
    /* Walk bit-by-bit; the buffer is MSB-first per byte already. */
    if (end_bit <= start_bit || bb == NULL || bb->buffer == NULL) {
        return LLC_CRC32_INIT ^ LLC_CRC32_XOROUT;
    }

    uint32_t crc = LLC_CRC32_INIT;
    for (size_t i = start_bit; i < end_bit; ++i) {
        const size_t  byte_idx = i >> 3;
        const uint8_t bit_off  = (uint8_t) (7u - (i & 0x7u));
        const uint32_t bit     = (uint32_t) ((bb->buffer[byte_idx] >> bit_off) & 0x1u);

        /* Reflected form, identical to tetra-zynq-phy decode_dl.py. */
        const uint32_t feedback = (bit ^ (crc & 0x1u)) & 0x1u;
        crc >>= 1;
        if (feedback) {
            crc ^= LLC_CRC32_POLY;
        }
    }
    return crc ^ LLC_CRC32_XOROUT;
}

/* Write `body_len_bits` from `body` into `bb`, MSB-first. */
static void put_body(BitBuffer *bb, const uint8_t *body, uint16_t body_len_bits)
{
    /* Whole bytes first, then a tail fragment if any. */
    uint16_t remaining = body_len_bits;
    size_t   src_bit   = 0;

    while (remaining >= 8u) {
        const size_t  byte_idx = src_bit >> 3;
        bb_put_bits(bb, (uint32_t) body[byte_idx], 8);
        src_bit   += 8u;
        remaining -= 8u;
    }
    if (remaining > 0u) {
        const size_t  byte_idx = src_bit >> 3;
        const uint8_t shift    = (uint8_t) (8u - remaining);
        const uint8_t mask     = (uint8_t) ((1u << remaining) - 1u);
        const uint32_t tail    = (uint32_t) ((body[byte_idx] >> shift) & mask);
        bb_put_bits(bb, tail, (uint8_t) remaining);
    }
}

/* Read `body_len_bits` from `bb` into `body`, MSB-first. */
static void get_body(BitBuffer *bb, uint8_t *body, uint16_t body_len_bits)
{
    /* We zero `body` first so the tail fragment lines up MSB-aligned. */
    const size_t bytes_full = (body_len_bits + 7u) / 8u;
    memset(body, 0, bytes_full);

    uint16_t remaining = body_len_bits;
    size_t   dst_bit   = 0;

    while (remaining >= 8u) {
        const uint32_t v       = bb_get_bits(bb, 8);
        const size_t   byte_idx = dst_bit >> 3;
        body[byte_idx] = (uint8_t) (v & 0xFFu);
        dst_bit   += 8u;
        remaining -= 8u;
    }
    if (remaining > 0u) {
        const uint32_t v        = bb_get_bits(bb, (uint8_t) remaining);
        const size_t   byte_idx = dst_bit >> 3;
        const uint8_t  shift    = (uint8_t) (8u - remaining);
        body[byte_idx] = (uint8_t) ((v & ((1u << remaining) - 1u)) << shift);
    }
}

/* ---------------------------------------------------------------------------
 * llc_pdu_encode — write LLC PDU at bb's cursor.
 *
 * Returns number of bits written, or negative errno on bad args.
 *
 * Layout (all MSB-first):
 *   pdu_type    [4 bits]
 *   NR          [1 bit]   if has_nr
 *   NS          [1 bit]   if has_ns
 *   body        [body_len_bits]
 *   FCS         [32 bits] if has_fcs (CRC-32 over the bits already written
 *                                      from start-of-PDU through end-of-body)
 *
 * AL-SETUP: pdu_type only, body_len_bits MUST be 0.
 * ------------------------------------------------------------------------- */
int llc_pdu_encode(BitBuffer *out, const LlcPdu *pdu)
{
    if (out == NULL || pdu == NULL) {
        return -EINVAL;
    }
    if (!llc_pdu_type_is_valid((uint8_t) pdu->pdu_type)) {
        return -EINVAL;
    }
    if (pdu->body_len_bits > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return -EINVAL;
    }
    if (pdu->pdu_type == LlcPdu_AL_SETUP && pdu->body_len_bits != 0u) {
        /* AL-SETUP is a wrapper only — bluestation never carries a body. */
        return -EINVAL;
    }

    const size_t pdu_start = bb_pos_bits(out) + out->start;

    /* pdu_type — 4 bits. */
    bb_put_bits(out, (uint32_t) pdu->pdu_type & 0xFu, 4);

    /* NR — 1 bit if applicable. */
    if (llc_pdu_type_has_nr(pdu->pdu_type)) {
        bb_put_bits(out, (uint32_t) pdu->nr & 0x1u, 1);
    }
    /* NS — 1 bit if applicable. */
    if (llc_pdu_type_has_ns(pdu->pdu_type)) {
        bb_put_bits(out, (uint32_t) pdu->ns & 0x1u, 1);
    }

    /* Body. */
    if (pdu->body_len_bits > 0u) {
        put_body(out, pdu->body, pdu->body_len_bits);
    }

    /* FCS — CRC-32 over [pdu_start..now). Note: the reference window is
     * the bytes already in the BitBuffer. */
    if (llc_pdu_type_has_fcs(pdu->pdu_type)) {
        const size_t pdu_end = bb_pos_bits(out) + out->start;
        const uint32_t fcs = crc32_over_bb_window(out, pdu_start, pdu_end);
        bb_put_bits(out, fcs, 32);
    }

    const size_t bits_written = (bb_pos_bits(out) + out->start) - pdu_start;
    return (int) bits_written;
}

/* ---------------------------------------------------------------------------
 * llc_pdu_decode — read LLC PDU at bb's cursor into `out`.
 *
 * Caller MUST set out->body_len_bits BEFORE calling — the LLC frame has no
 * length self-description on-air; callers know body_len_bits from the MAC
 * length-indication. Pass 0 for AL-SETUP.
 *
 * On success returns 0 and fills `out`. On failure returns -EINVAL/-EPROTO.
 * Sets out->fcs_valid only for FCS-bearing types.
 * ------------------------------------------------------------------------- */
int llc_pdu_decode(BitBuffer *in, LlcPdu *out)
{
    if (in == NULL || out == NULL) {
        return -EINVAL;
    }
    if (out->body_len_bits > LLC_PDU_BODY_MAX_BYTES * 8u) {
        return -EINVAL;
    }

    const size_t pdu_start = bb_pos_bits(in) + in->start;

    /* Need at least 4 bits for pdu_type. */
    if (bb_remaining(in) < 4u) {
        return -EPROTO;
    }
    const uint32_t raw_type = bb_get_bits(in, 4);
    if (!llc_pdu_type_is_valid((uint8_t) raw_type)) {
        out->pdu_type = LlcPdu_Unknown;
        return -EPROTO;
    }
    out->pdu_type = (LlcPduType) raw_type;
    out->nr       = 0;
    out->ns       = 0;
    out->fcs      = 0;
    out->fcs_valid = false;

    /* AL-SETUP: nothing else to consume. */
    if (out->pdu_type == LlcPdu_AL_SETUP) {
        out->body_len_bits = 0;
        return 0;
    }

    /* NR/NS as required by type. */
    const size_t need_ctrl = (size_t) (llc_pdu_type_has_nr(out->pdu_type) ? 1u : 0u) +
                             (size_t) (llc_pdu_type_has_ns(out->pdu_type) ? 1u : 0u);
    if (bb_remaining(in) < need_ctrl) {
        return -EPROTO;
    }
    if (llc_pdu_type_has_nr(out->pdu_type)) {
        out->nr = (uint8_t) bb_get_bits(in, 1);
    }
    if (llc_pdu_type_has_ns(out->pdu_type)) {
        out->ns = (uint8_t) bb_get_bits(in, 1);
    }

    /* Body. body_len_bits is caller-supplied. */
    if (out->body_len_bits > 0u) {
        if (bb_remaining(in) < out->body_len_bits) {
            return -EPROTO;
        }
        get_body(in, out->body, out->body_len_bits);
    }

    /* FCS if applicable. The CRC covers bits [pdu_start..pre-FCS). */
    if (llc_pdu_type_has_fcs(out->pdu_type)) {
        if (bb_remaining(in) < 32u) {
            return -EPROTO;
        }
        const size_t pre_fcs = bb_pos_bits(in) + in->start;
        const uint32_t want  = crc32_over_bb_window(in, pdu_start, pre_fcs);
        const uint32_t got   = bb_get_bits(in, 32);
        out->fcs       = got;
        out->fcs_valid = (got == want);
    }

    return 0;
}

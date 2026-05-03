/* sw/core/src/bitbuffer.c — MSB-first bit cursor.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Locked under IF_CORE_API_v1.
 *
 * Mirrors bluestation tetra-core/src/bitbuffer.rs in semantics:
 *   - MSB-first within each byte. Bit 0 of a byte is the MSB.
 *   - Cursor is absolute over `buffer`; window is [start..end).
 *   - Writes that exceed `end` panic via assert when autoexpand is OFF;
 *     when ON, `end` advances up to `cap_bits` (no realloc — the
 *     SW-side caller hands us the buffer, this implementation never
 *     malloc()s).
 *
 * Bit-exact target: see test_core_msgbus.c for the
 * reference_demand_reassembly_bitexact.md UL#0 first 32 bits =
 * 0x01_41_7F_A7 round-trip.
 *
 * The 1..32 width range in the public API is enough for every TETRA
 * field encountered in the SAP-Layer encoders (largest single field
 * is the 24-bit SSI). The internal _at primitives accept up to 32 too;
 * 64-bit reads are not exposed because no current consumer needs them.
 */
#include "tetra/msgbus.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal raw read/write at absolute bit position.
 * ------------------------------------------------------------------------- */

static uint32_t bb_read_at(const BitBuffer *bb, size_t bit_pos, uint8_t n)
{
    /* MSB-first: bit at offset b within byte at index B is the
     * (7 - (b mod 8))-th LSB of buffer[B]. Walk byte-by-byte from the
     * head fragment, through whole bytes, to the tail fragment. */
    uint32_t result        = 0;
    size_t   bits_remaining = n;
    size_t   cur            = bit_pos;

    /* Head bits — align to next byte boundary if mid-byte. */
    const size_t head = cur & 0x7u;
    if (head != 0u && bits_remaining > 0u) {
        const size_t take = (8u - head < bits_remaining) ? (8u - head) : bits_remaining;
        const uint8_t byte = bb->buffer[cur >> 3];
        const size_t  shift = 8u - head - take;
        const uint8_t mask  = (uint8_t) ((1u << take) - 1u);
        result = (uint32_t) ((byte >> shift) & mask);
        cur            += take;
        bits_remaining -= take;
    }

    /* Whole bytes. */
    while (bits_remaining >= 8u) {
        const uint8_t byte = bb->buffer[cur >> 3];
        result          = (result << 8) | (uint32_t) byte;
        cur            += 8u;
        bits_remaining -= 8u;
    }

    /* Tail bits — partial byte at the end. */
    if (bits_remaining > 0u) {
        const uint8_t byte  = bb->buffer[cur >> 3];
        const size_t  shift = 8u - bits_remaining;
        const uint8_t mask  = (uint8_t) ((1u << bits_remaining) - 1u);
        result = (result << bits_remaining) |
                 (uint32_t) ((byte >> shift) & mask);
    }

    return result;
}

static void bb_write_at(BitBuffer *bb, size_t bit_pos, uint32_t v, uint8_t n)
{
    /* Same chunking strategy as bb_read_at, but masking the destination
     * byte so that we don't disturb neighbouring bits. */
    size_t  bits_remaining = n;
    size_t  cur            = bit_pos;
    /* mask v to its low n bits — caller is asserted to have done this
     * already, but defensive masking has no cost. */
    const uint32_t v_masked = (n == 32u) ? v : (v & ((1u << n) - 1u));

    /* Head fragment. */
    const size_t head = cur & 0x7u;
    if (head != 0u && bits_remaining > 0u) {
        const size_t take = (8u - head < bits_remaining) ? (8u - head) : bits_remaining;
        const size_t shift = 8u - head - take;
        const uint8_t mask = (uint8_t) (((1u << take) - 1u) << shift);
        const uint8_t bits = (uint8_t) (((v_masked >> (bits_remaining - take)) &
                                         ((1u << take) - 1u)) << shift);
        uint8_t *p = &bb->buffer[cur >> 3];
        *p = (uint8_t) ((*p & (uint8_t) ~mask) | bits);
        cur            += take;
        bits_remaining -= take;
    }

    /* Whole bytes. */
    while (bits_remaining >= 8u) {
        const uint8_t byte = (uint8_t) ((v_masked >> (bits_remaining - 8u)) & 0xFFu);
        bb->buffer[cur >> 3] = byte;
        cur            += 8u;
        bits_remaining -= 8u;
    }

    /* Tail fragment. */
    if (bits_remaining > 0u) {
        const size_t shift = 8u - bits_remaining;
        const uint8_t mask = (uint8_t) (((1u << bits_remaining) - 1u) << shift);
        const uint8_t bits = (uint8_t) ((v_masked & ((1u << bits_remaining) - 1u)) << shift);
        uint8_t *p = &bb->buffer[cur >> 3];
        *p = (uint8_t) ((*p & (uint8_t) ~mask) | bits);
    }
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

BitBuffer bb_init(uint8_t *buf, size_t len_bits)
{
    BitBuffer bb = {
        .buffer     = buf,
        .cap_bits   = len_bits,
        .start      = 0,
        .pos        = 0,
        .end        = len_bits,
        .autoexpand = false,
    };
    return bb;
}

BitBuffer bb_init_autoexpand(uint8_t *buf, size_t cap_bits)
{
    BitBuffer bb = {
        .buffer     = buf,
        .cap_bits   = cap_bits,
        .start      = 0,
        .pos        = 0,
        .end        = 0,
        .autoexpand = true,
    };
    return bb;
}

void bb_set_autoexpand(BitBuffer *bb, bool enable)
{
    if (bb == NULL) {
        return;
    }
    bb->autoexpand = enable;
}

void bb_put_bits(BitBuffer *bb, uint32_t v, uint8_t n)
{
    assert(bb != NULL);
    assert(bb->buffer != NULL);
    assert(n >= 1u && n <= 32u);
    /* Reject values that exceed the requested width — same check as
     * bluestation's `value exceeds num_bits` panic. Catches the common
     * "I forgot to mask" bug at the SAP encoder level. */
    if (n < 32u) {
        assert((v >> n) == 0u);
    }

    const size_t need_end = bb->pos + n;
    if (need_end > bb->end) {
        if (bb->autoexpand) {
            assert(need_end <= bb->cap_bits);
            bb->end = need_end;
        } else {
            assert(false && "bb_put_bits would exceed buffer end");
        }
    }

    bb_write_at(bb, bb->pos, v, n);
    bb->pos += n;
}

uint32_t bb_get_bits(BitBuffer *bb, uint8_t n)
{
    assert(bb != NULL);
    assert(bb->buffer != NULL);
    assert(n >= 1u && n <= 32u);

    if (bb->pos + n > bb->end) {
        /* Underrun — return 0 rather than aborting. Callers that care
         * check bb_remaining() first; this matches bluestation's
         * Option<u64> -> None semantics, mapped to 0 in C-land. */
        return 0u;
    }

    const uint32_t v = bb_read_at(bb, bb->pos, n);
    bb->pos += n;
    return v;
}

size_t bb_pos_bits(const BitBuffer *bb)
{
    if (bb == NULL) {
        return 0;
    }
    return bb->pos - bb->start;
}

void bb_seek_bits(BitBuffer *bb, size_t pos)
{
    assert(bb != NULL);
    const size_t abs = bb->start + pos;
    assert(abs <= bb->end);
    bb->pos = abs;
}

size_t bb_remaining(const BitBuffer *bb)
{
    if (bb == NULL) {
        return 0;
    }
    return bb->end - bb->pos;
}

size_t bb_len_bits(const BitBuffer *bb)
{
    if (bb == NULL) {
        return 0;
    }
    return bb->end - bb->start;
}

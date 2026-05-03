/* tb/sw/mm/test_mm_iep_d_attach_grp_ack.c — Group-Attach ACK bit-exact.
 *
 * Test gate per docs/MIGRATION_PLAN.md §S3:
 *   build D-ATTACH-DETACH-GRP-ID-ACK for Gold Group-Attach.
 *   0 bit diff vs reference_group_attach_bitexact.md DL slices #1..#5.
 *
 * Each slice is 124 bits = MAC-RESOURCE(43) + LLC-ADATA(4) + NR(1) + NS(1) +
 * MLE-PD(3) + MM body(62) + 2 fill bits. The MM-layer builder produces only
 * the 62-bit MM body; this test wraps it into the full 124-bit frame for
 * comparison. Wrapping logic mirrors reference_group_attach_bitexact.md
 * "DL-ACK Layout" exactly.
 */

#include "tetra/llc.h"
#include "tetra/mm.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Gold-Ref slices from reference_group_attach_bitexact.md (lines 113-122). */
typedef struct {
    const char *bits;
    uint8_t     nr;
    uint8_t     ns;
    uint32_t    gssi;
} GoldSlice;

static const GoldSlice GOLD_SLICES[] = {
    /* slice #1 — NR=0 NS=1 */
    { "0010000010000001001010000010111111110100010000000000000010011011001101110000010011000000100110000001011110100110101100011010", 0, 1, 0x2F4D63u },
    /* slice #2 — NR=1 NS=0 */
    { "0010000010000001001010000010111111110100010000000000000100011011001101110000010011000000100110000001011110100110101100010010", 1, 0, 0x2F4D62u },
    /* slice #3 — NR=0 NS=1 */
    { "0010000010000001001010000010111111110100010000000000000010011011001101110000010011000000100110000001011110100110101100001010", 0, 1, 0x2F4D61u },
    /* slice #4 — NR=1 NS=0 (identical to #2) */
    { "0010000010000001001010000010111111110100010000000000000100011011001101110000010011000000100110000001011110100110101100010010", 1, 0, 0x2F4D62u },
    /* slice #5 — NR=0 NS=1 (identical to #1) */
    { "0010000010000001001010000010111111110100010000000000000010011011001101110000010011000000100110000001011110100110101100011010", 0, 1, 0x2F4D63u },
};

/* ---------------------------------------------------------------------------
 * Bit-buffer helper (MSB-first into byte array).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *bits;
    size_t   pos;
    size_t   cap_bits;
} W;

static void put_bits(W *w, uint64_t v, uint8_t n)
{
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t b = (uint8_t) ((v >> (n - 1u - i)) & 0x1u);
        if (b) {
            const size_t  by = w->pos >> 3;
            const uint8_t off = (uint8_t) (7u - (w->pos & 0x7u));
            w->bits[by] |= (uint8_t) (1u << off);
        }
        ++w->pos;
    }
}

static void put_bitstream(W *w, const uint8_t *src, size_t src_bits)
{
    for (size_t i = 0; i < src_bits; ++i) {
        const size_t  sb = i >> 3;
        const uint8_t soff = (uint8_t) (7u - (i & 0x7u));
        const uint8_t b = (uint8_t) ((src[sb] >> soff) & 0x1u);
        if (b) {
            const size_t  db = w->pos >> 3;
            const uint8_t doff = (uint8_t) (7u - (w->pos & 0x7u));
            w->bits[db] |= (uint8_t) (1u << doff);
        }
        ++w->pos;
    }
}

/* Convert a 124-char "01"-string to byte array, MSB-first. */
static void bitstr_to_bytes(const char *s, uint8_t *out)
{
    const size_t n = strlen(s);
    memset(out, 0, (n + 7u) / 8u);
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '1') {
            const size_t  b = i >> 3;
            const uint8_t off = (uint8_t) (7u - (i & 0x7u));
            out[b] |= (uint8_t) (1u << off);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Build the full 124-bit Gold-Ref-shaped frame from the MM body.
 *
 *   MAC-RESOURCE: pdu_type=00, fill=1, PoG=0, enc=00, RA=0, LI=16,
 *                 addr_type=001, SSI=0x282FF4, pwr=0, sg=1, sg_elem=0x00,
 *                 ca_flag=0
 *   LLC: pdu_type=0000 (BL-ADATA), NR, NS
 *   MLE: disc=001 (MM)
 *   MM body (62 bits) — caller-supplied
 *   Trailing fill bits to 124: first fill bit = 1, rest = 0 (fill_bit_ind=1)
 * ------------------------------------------------------------------------- */
static void build_full_frame(uint8_t *out_124,
                             uint8_t nr, uint8_t ns,
                             const uint8_t *mm_body, size_t mm_bits)
{
    memset(out_124, 0, 16);  /* 124 bits → 16 bytes pre-zeroed */
    W w = { .bits = out_124, .pos = 0, .cap_bits = 128u };

    /* MAC-RESOURCE header (43 bits). */
    put_bits(&w, 0x0u, 2);             /* pdu_type=00 */
    put_bits(&w, 1u, 1);               /* fill_bit=1 */
    put_bits(&w, 0u, 1);               /* PoG=0 */
    put_bits(&w, 0u, 2);               /* encryption=00 */
    put_bits(&w, 0u, 1);               /* random_access_flag=0 */
    put_bits(&w, 16u, 6);              /* LI=16 */
    put_bits(&w, 0x1u, 3);             /* addr_type=001 (SSI) */
    put_bits(&w, 0x282FF4u, 24);       /* SSI */
    put_bits(&w, 0u, 1);               /* pwr_flag=0 */
    put_bits(&w, 1u, 1);               /* slot_grant_flag=1 */
    put_bits(&w, 0u, 8);               /* slot_grant_elem=0x00 */
    put_bits(&w, 0u, 1);               /* ca_flag=0 */

    /* LLC: pdu_type=0000 (BL-ADATA), NR, NS. */
    put_bits(&w, 0u, 4);
    put_bits(&w, nr, 1);
    put_bits(&w, ns, 1);

    /* MLE-disc=001. */
    put_bits(&w, 0x1u, 3);

    /* MM body. */
    put_bitstream(&w, mm_body, mm_bits);

    /* Fill bits (first=1, rest=0). 124 - w.pos remaining. */
    if (w.pos < 124u) {
        put_bits(&w, 1u, 1);
        while (w.pos < 124u) {
            put_bits(&w, 0u, 1);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Tests.
 * ------------------------------------------------------------------------- */

void setUp(void) {}
void tearDown(void) {}

static void check_slice(const GoldSlice *gs)
{
    /* Build the MM body via the builder. */
    MmGrpAckParams p = {0};
    p.accept_reject       = 0;
    p.gid_downlink_present = true;
    p.num_gid             = 1;
    p.gid[0].is_attach           = true;
    p.gid[0].attach.lifetime     = 1;
    p.gid[0].attach.class_of_usage = 4;
    p.gid[0].address_type        = 0;
    p.gid[0].gssi                = gs->gssi;

    uint8_t mm_body[16] = {0};
    int mm_bits = mm_build_d_attach_detach_grp_id_ack(mm_body,
                                                     sizeof(mm_body) * 8u, &p);
    TEST_ASSERT_GREATER_OR_EQUAL(0, mm_bits);
    TEST_ASSERT_EQUAL_INT(62, mm_bits);

    /* Wrap into full 124-bit frame. */
    uint8_t got[16] = {0};
    build_full_frame(got, gs->nr, gs->ns, mm_body, (size_t) mm_bits);

    /* Convert reference string to bytes. */
    uint8_t ref[16] = {0};
    bitstr_to_bytes(gs->bits, ref);

    /* Compare 124 bits — first 15 bytes (120 bits) plus high 4 bits of byte 15. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ref, got, 15);
    /* Last 4 bits in byte 15 (bits 120..123) — mask the low 4 bits since
     * they're below the 124-bit boundary. */
    TEST_ASSERT_EQUAL_HEX8(ref[15] & 0xF0u, got[15] & 0xF0u);
}

static void test_grp_ack_slice1(void) { check_slice(&GOLD_SLICES[0]); }
static void test_grp_ack_slice2(void) { check_slice(&GOLD_SLICES[1]); }
static void test_grp_ack_slice3(void) { check_slice(&GOLD_SLICES[2]); }
static void test_grp_ack_slice4(void) { check_slice(&GOLD_SLICES[3]); }
static void test_grp_ack_slice5(void) { check_slice(&GOLD_SLICES[4]); }

static void test_grp_ack_mm_body_only(void)
{
    /* Sanity: a minimal accept (no GIDs) should yield 8-bit body. */
    MmGrpAckParams p = {0};
    p.accept_reject       = 1;
    p.gid_downlink_present = false;
    p.num_gid             = 0;

    uint8_t out[8] = {0};
    int n = mm_build_d_attach_detach_grp_id_ack(out, sizeof(out) * 8u, &p);
    TEST_ASSERT_EQUAL_INT(7, n);
    /* 1011 1 0 0 = pdu_type=11, accept_reject=1, reserved=0, obit=0 */
    TEST_ASSERT_EQUAL_HEX8(0xB8u, out[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_grp_ack_slice1);
    RUN_TEST(test_grp_ack_slice2);
    RUN_TEST(test_grp_ack_slice3);
    RUN_TEST(test_grp_ack_slice4);
    RUN_TEST(test_grp_ack_slice5);
    RUN_TEST(test_grp_ack_mm_body_only);
    return UNITY_END();
}

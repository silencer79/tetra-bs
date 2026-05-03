/* tb/sw/mm/test_mm_d_loc_update_accept.c — S3 D-LOC-UPDATE-ACCEPT bit-exact.
 *
 * Test gate per docs/MIGRATION_PLAN.md §S3:
 *   build D-LOC-UPDATE-ACCEPT for Gold M2 attach.
 *   0/102 bit diff vs DL#735 MM body (Z.111-152).
 */

#include "tetra/mm.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Build the reference 102-bit MM body programmatically per
 * reference_gold_attach_bitexact.md Z.111-152. Hand-coded bit string would
 * be error-prone. */

static void build_ref_mm_body(uint8_t *out, size_t cap_bits, size_t *out_bits)
{
    /* Write MSB-first into out[]. */
    memset(out, 0, (cap_bits + 7u) / 8u);
    size_t pos = 0;
    /* helper */
#define PUT(v, n)                                                          \
    do {                                                                   \
        const uint64_t _val = (uint64_t)(v);                                \
        for (uint8_t _i = 0; _i < (uint8_t)(n); ++_i) {                     \
            const uint8_t _b = (uint8_t)((_val >> ((n) - 1u - _i)) & 0x1u); \
            const size_t  _byte = (pos) >> 3;                               \
            const uint8_t _off  = (uint8_t)(7u - ((pos) & 0x7u));            \
            if (_b) out[_byte] |= (uint8_t)(1u << _off);                    \
            ++pos;                                                          \
        }                                                                   \
    } while (0)

    /* Header. */
    PUT(0x5, 4);   /* mm_pdu_type = 0101 */
    PUT(0x3, 3);   /* loc_acc_type = 011 (ITSI) */
    PUT(1, 1);     /* o-bit */

    /* Type-2 fields (p-bits, all 0 except energy_saving). */
    PUT(0, 1);     /* p_ssi = 0 */
    PUT(0, 1);     /* p_ae = 0 */
    PUT(0, 1);     /* p_subs = 0 */
    PUT(1, 1);     /* p_esi = 1 */
    PUT(0, 3);     /* energy_saving_mode = StayAlive (=0) */
    PUT(0, 5);     /* esi frame_number = 0 */
    PUT(0, 6);     /* esi multiframe = 0 */
    PUT(0, 1);     /* p_scch = 0 */

    /* Type-3 GILA. */
    PUT(1, 1);     /* m-bit */
    PUT(0x5, 4);   /* elem_id = 0101 (=5, GroupIdentityLocationAccept) */
    PUT(58, 11);   /* length = 58 */
    /* GILA payload */
    PUT(0, 1);     /* group_identity_accept_reject = 0 */
    PUT(0, 1);     /* reserved = 0 */
    PUT(1, 1);     /* obit (inner) = 1 */
    /* GID-Downlink Type-4 */
    PUT(1, 1);     /* m-bit */
    PUT(0x7, 4);   /* elem_id = 0111 (=7, GroupIdentityDownlink) */
    PUT(38, 11);   /* length = 38 */
    PUT(1, 6);     /* num_elems = 1 */
    /* GID-Downlink struct (32 bits) */
    PUT(0, 1);     /* attach_detach_type_id = 0 (attach) */
    PUT(1, 2);     /* lifetime = 01 = 1 */
    PUT(4, 3);     /* class_of_usage = 100 = 4 */
    PUT(0, 2);     /* address_type = 00 (GSSI) */
    PUT(0x2F4D61u, 24); /* gssi = 0x2F4D61 */
    PUT(0, 1);     /* GILA inner trailing m-bit */
    /* MM body trailing m-bit. */
    PUT(0, 1);

#undef PUT
    *out_bits = pos;
}

/* ---------------------------------------------------------------------------
 * Tests.
 * ------------------------------------------------------------------------- */

void setUp(void) {}
void tearDown(void) {}

static void test_build_d_loc_update_accept_gold_m2(void)
{
    /* Build reference. */
    uint8_t ref[64];
    size_t  ref_bits = 0;
    build_ref_mm_body(ref, sizeof(ref) * 8u, &ref_bits);
    TEST_ASSERT_EQUAL_UINT(102u, ref_bits);

    /* Configure MmAcceptParams to match Gold-Ref M2. */
    MmAcceptParams p = {0};
    p.accept_type                    = LocUpdate_ItsiAttach;
    p.energy_saving_info_present     = true;
    p.energy_saving_mode             = EnergySaving_StayAlive;
    p.energy_saving_frame_number     = 0;
    p.energy_saving_multiframe       = 0;
    p.gila_present                   = true;
    p.gila_accept_reject             = 0;
    p.num_gid                        = 1;
    p.gid[0].is_attach               = true;
    p.gid[0].attach.lifetime         = 1;
    p.gid[0].attach.class_of_usage   = 4;
    p.gid[0].address_type            = 0;  /* GSSI only */
    p.gid[0].gssi                    = 0x2F4D61u;

    /* Build via MM. */
    uint8_t got[64];
    int n = mm_build_d_loc_update_accept(got, sizeof(got) * 8u, &p);
    TEST_ASSERT_GREATER_OR_EQUAL(0, n);
    TEST_ASSERT_EQUAL_INT(102, n);

    /* Compare bit-exact. */
    const size_t bytes = (ref_bits + 7u) / 8u;
    /* Mask the last byte to ignore tail bits. */
    const size_t tail_bits = ref_bits & 0x7u;
    if (tail_bits) {
        const uint8_t mask = (uint8_t) ((0xFFu << (8u - tail_bits)) & 0xFFu);
        TEST_ASSERT_EQUAL_HEX8(ref[bytes - 1] & mask,
                               got[bytes - 1] & mask);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ref, got, bytes - 1);
    } else {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ref, got, bytes);
    }
}

static void test_build_d_loc_update_accept_no_obit(void)
{
    /* No optional fields → just header + obit=0. */
    MmAcceptParams p = {0};
    p.accept_type = LocUpdate_ItsiAttach;
    /* nothing else present */

    uint8_t got[16] = {0};
    int n = mm_build_d_loc_update_accept(got, sizeof(got) * 8u, &p);
    TEST_ASSERT_EQUAL_INT(8, n);  /* 4 + 3 + 1 = 8 bits */
    /* 01010110 = pdu_type=5, loc_acc=3, obit=0 */
    TEST_ASSERT_EQUAL_HEX8(0x56, got[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_d_loc_update_accept_gold_m2);
    RUN_TEST(test_build_d_loc_update_accept_no_obit);
    return UNITY_END();
}

/* tb/sw/mm/test_mm_iep_decode.c — S3 IE-Parser decode tests.
 *
 * Test gate per docs/MIGRATION_PLAN.md §S3:
 *   parse Gold UL#0+UL#1 reassembled MM body, assert decoded fields match
 *   gold_field_values.md U-LOC-UPDATE-DEMAND row.
 *
 * Reassembly: per bluestation `from_bitbuf` convention, the MM body
 * includes the 4-bit mm_pdu_type at bit 0, so we reassemble 4 bits of
 * mm_pdu_type from ul0[44..47] plus the body content from
 * ul0[48..91] (44 bits) ++ ul1[7..91] (85 bits) = 133 bits total.
 * (The "129-bit MM body" wording in
 * docs/references/reference_demand_reassembly_bitexact.md Z.16 omits
 * mm_pdu_type from the count; bluestation's parser includes it.)
 *
 * Field values verified against gold_field_values.md (and corrected via
 * fresh bit-decode 2026-05-03):
 *   location_update_type      = ItsiAttach (=3)
 *   class_of_ms               = 0x1A1060 (per task description)
 *   energy_saving_mode        = EnergyEconomy1 (=1)
 *   GILD attach_detach_mode   = 1 (replace-all per Gold UL bit 41)
 *   GILD GIU class_of_usage   = 4
 *   GILD GIU GSSI             = 0x2F4D61
 */

#include "tetra/mm.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Helper: write `n` MSB-first bits of `v` at byte/bit offset `pos` of `out`. */
static void put_bits(uint8_t *out, size_t *pos, uint64_t v, uint8_t n)
{
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t b = (uint8_t) ((v >> (n - 1u - i)) & 0x1u);
        if (b) {
            const size_t  by = (*pos) >> 3;
            const uint8_t off = (uint8_t) (7u - ((*pos) & 0x7u));
            out[by] |= (uint8_t) (1u << off);
        }
        ++(*pos);
    }
}

static void put_bytes(uint8_t *out, size_t *pos,
                      const uint8_t *src, size_t src_bits)
{
    for (size_t i = 0; i < src_bits; ++i) {
        const size_t  sb = i >> 3;
        const uint8_t soff = (uint8_t) (7u - (i & 0x7u));
        const uint8_t b = (uint8_t) ((src[sb] >> soff) & 0x1u);
        if (b) {
            const size_t  db = (*pos) >> 3;
            const uint8_t doff = (uint8_t) (7u - ((*pos) & 0x7u));
            out[db] |= (uint8_t) (1u << doff);
        }
        ++(*pos);
    }
}

/* Reassemble a 133-bit MM body from the Gold-Ref UL#0 + UL#1 hex. */
static size_t reassemble_gold_demand(uint8_t *out, size_t out_cap_bits)
{
    /* UL#0 hex: 01 41 7F A7 01 12 66 34 20 C1 22 60 (12 bytes = 96 bits) */
    static const uint8_t ul0[12] = {
        0x01, 0x41, 0x7F, 0xA7, 0x01, 0x12, 0x66, 0x34, 0x20, 0xC1, 0x22, 0x60
    };
    /* UL#1 hex: D4 1C 3C 02 40 50 2F 4D 61 20 00 00 */
    static const uint8_t ul1[12] = {
        0xD4, 0x1C, 0x3C, 0x02, 0x40, 0x50, 0x2F, 0x4D, 0x61, 0x20, 0x00, 0x00
    };

    memset(out, 0, (out_cap_bits + 7u) / 8u);
    size_t pos = 0;

    /* ul0[44..91] = 48 bits incl. mm_type at bits 44..47. */
    for (size_t i = 44; i < 92; ++i) {
        const size_t  sb = i >> 3;
        const uint8_t soff = (uint8_t) (7u - (i & 0x7u));
        const uint8_t b = (uint8_t) ((ul0[sb] >> soff) & 0x1u);
        put_bits(out, &pos, b, 1);
    }
    /* ul1[7..91] = 85 bits. */
    for (size_t i = 7; i < 92; ++i) {
        const size_t  sb = i >> 3;
        const uint8_t soff = (uint8_t) (7u - (i & 0x7u));
        const uint8_t b = (uint8_t) ((ul1[sb] >> soff) & 0x1u);
        put_bits(out, &pos, b, 1);
    }
    (void) put_bytes; /* silence unused-static warning if any */
    return pos;  /* should be 48+85 = 133 */
}

/* ---------------------------------------------------------------------------
 * Tests.
 * ------------------------------------------------------------------------- */

void setUp(void) {}
void tearDown(void) {}

static void test_decode_gold_u_loc_update_demand(void)
{
    uint8_t body[24] = {0};
    const size_t bits = reassemble_gold_demand(body, sizeof(body) * 8u);
    TEST_ASSERT_EQUAL_UINT(133u, bits);

    MmDecoded d;
    int rc = mm_iep_decode(body, bits, &d);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_UINT8(MmPduUl_ULocationUpdateDemand, d.pdu_type);
    TEST_ASSERT_EQUAL_UINT8(LocUpdate_ItsiAttach, d.location_update_type);
    TEST_ASSERT_FALSE(d.request_to_append_la);
    TEST_ASSERT_FALSE(d.cipher_control);

    /* gold_field_values.md: class_of_ms = 0x1A1060 (from task description),
     * decoded bit-exactly from the Gold reassembled body. */
    TEST_ASSERT_TRUE(d.class_of_ms_present);
    TEST_ASSERT_EQUAL_HEX32(0x001A1060u, d.class_of_ms);

    /* gold_field_values.md: energy_saving_mode = EnergyEconomy1 (=1). */
    TEST_ASSERT_TRUE(d.energy_saving_mode_present);
    TEST_ASSERT_EQUAL_UINT8(EnergySaving_EnergyEconomy1, d.energy_saving_mode);

    /* la_information / ssi / address_extension all None. */
    TEST_ASSERT_FALSE(d.la_information_present);
    TEST_ASSERT_FALSE(d.ssi_present);
    TEST_ASSERT_FALSE(d.address_extension_present);

    /* GILD present with GSSI=0x2F4D61. */
    TEST_ASSERT_TRUE(d.gild_present);
    TEST_ASSERT_EQUAL_UINT8(1, d.gild.attach_detach_mode);  /* mode=1 (replace-all) */
    TEST_ASSERT_EQUAL_UINT8(1, d.gild.num_giu);
    TEST_ASSERT_TRUE(d.gild.giu[0].is_attach);
    TEST_ASSERT_EQUAL_UINT8(4, d.gild.giu[0].class_of_usage);
    TEST_ASSERT_EQUAL_UINT8(0, d.gild.giu[0].address_type);
    TEST_ASSERT_EQUAL_HEX32(0x002F4D61u, d.gild.giu[0].gssi);
}

static void test_decode_short_body_eproto(void)
{
    uint8_t body[2] = {0};
    MmDecoded d;
    int rc = mm_iep_decode(body, 4u, &d);
    /* 4 bits = pdu_type only; not enough for even location_update_type. */
    TEST_ASSERT_LESS_THAN(0, rc);
}

static void test_decode_unknown_pdu_type(void)
{
    /* pdu_type = 0xF (function-not-supported style) — decoder returns ENOTSUP. */
    uint8_t body[8] = {0xF0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    MmDecoded d;
    int rc = mm_iep_decode(body, 64u, &d);
    TEST_ASSERT_LESS_THAN(0, rc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_gold_u_loc_update_demand);
    RUN_TEST(test_decode_short_body_eproto);
    RUN_TEST(test_decode_unknown_pdu_type);
    return UNITY_END();
}

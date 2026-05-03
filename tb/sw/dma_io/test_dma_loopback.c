/* tb/sw/dma_io/test_dma_loopback.c — S1 host unit tests.
 *
 * Owned by S1 (S1-sw-dma-glue). Test-gate per
 * docs/MIGRATION_PLAN.md §S1:
 *
 *   - 32-byte TMAS round-trip on TMA_TX (send) → TMA_RX (recv via mock-inject)
 *   - 44-byte TMDC round-trip on TMD_TX (send) → TMD_RX (recv via mock-inject)
 *   - 12-byte TMAR (FPGA-side report) round-trip via mock-inject on TMA_RX
 *   - bad-magic on RX  → -EBADMSG, drop_bad_magic increments
 *   - length overflow  → -EBADMSG, drop_bad_length increments
 *   - declared length > available bytes → -EBADMSG via frame_parse_header
 *   - partial frame split across 2 reads → reassembly correct
 *   - timeout 0 with empty pipe → returns 0 (no error, no data)
 *   - back-to-back frames in one read → both extract on successive recv calls
 *   - TX magic auto-selection (TMA_TX → TMAS, TMD_TX → TMDC)
 *   - reject send on RX channel → -EINVAL
 *
 * Backend: mock (pipe-pair). Host x86 build never has HAVE_LIBAXIDMA
 * defined (CI is Ubuntu 24.04, libaxidma not packaged). The real-HW
 * path is unit-tested in T2 cosim and Phase-4 live-air.
 */

#include "tetra/dma_io.h"
#include "dma_io_internal.h"
#include "unity.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>  /* read() in TX→RX bridge for the loopback tests */

/* ---------------------------------------------------------------------------
 * Common scaffolding.
 * ------------------------------------------------------------------------- */

static DmaCtx g_ctx;

void setUp(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    DmaCfg cfg = { .force_mock = true };
    const int rc = dma_init(&g_ctx, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void tearDown(void)
{
    dma_close(&g_ctx);
}

/* Build an on-the-wire frame: magic(4 BE) + len(4 BE) + payload. */
static void build_frame(uint8_t *out,
                        uint32_t magic,
                        const uint8_t *payload,
                        uint32_t plen)
{
    out[0] = (uint8_t) (magic >> 24);
    out[1] = (uint8_t) (magic >> 16);
    out[2] = (uint8_t) (magic >>  8);
    out[3] = (uint8_t) (magic >>  0);
    out[4] = (uint8_t) (plen  >> 24);
    out[5] = (uint8_t) (plen  >> 16);
    out[6] = (uint8_t) (plen  >>  8);
    out[7] = (uint8_t) (plen  >>  0);
    if (plen > 0 && payload != NULL) {
        memcpy(out + 8, payload, plen);
    }
}

/* ---------------------------------------------------------------------------
 * Test 1 — frame_magic_is_known + frame_parse_header round-trip.
 * Pure parser, no DMA ctx needed.
 * ------------------------------------------------------------------------- */
static void test_frame_parse_header_happy(void)
{
    uint8_t buf[8 + 4];
    const uint8_t pl[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    build_frame(buf, FRAME_MAGIC_TMAS, pl, 4);

    FrameMagic m  = (FrameMagic) 0;
    uint32_t   pl_len = 0;
    TEST_ASSERT_EQUAL_INT(0, frame_parse_header(buf, sizeof(buf), &m, &pl_len));
    TEST_ASSERT_EQUAL_HEX32(FRAME_MAGIC_TMAS, (uint32_t) m);
    TEST_ASSERT_EQUAL_UINT32(4, pl_len);

    TEST_ASSERT_TRUE(frame_magic_is_known(FRAME_MAGIC_TMAS));
    TEST_ASSERT_TRUE(frame_magic_is_known(FRAME_MAGIC_TMAR));
    TEST_ASSERT_TRUE(frame_magic_is_known(FRAME_MAGIC_TMDC));
    TEST_ASSERT_FALSE(frame_magic_is_known(0xDEADBEEFu));
}

static void test_frame_parse_header_short_buf(void)
{
    uint8_t hdr[7] = { 'T','M','A','S', 0,0,0 };  /* 7 bytes < header size */
    FrameMagic m;
    uint32_t   pl_len;
    TEST_ASSERT_EQUAL_INT(-EBADMSG,
                          frame_parse_header(hdr, sizeof(hdr), &m, &pl_len));
}

static void test_frame_parse_header_bad_magic(void)
{
    uint8_t buf[8] = { 'X','Y','Z','W', 0,0,0,0 };
    FrameMagic m;
    uint32_t   pl_len;
    TEST_ASSERT_EQUAL_INT(-EBADMSG,
                          frame_parse_header(buf, sizeof(buf), &m, &pl_len));
}

static void test_frame_parse_header_len_mismatch(void)
{
    /* declared 100 bytes payload, but supplied buffer only carries 8 hdr. */
    uint8_t buf[8] = { 'T','M','A','S', 0, 0, 0, 100 };
    FrameMagic m;
    uint32_t   pl_len;
    TEST_ASSERT_EQUAL_INT(-EBADMSG,
                          frame_parse_header(buf, sizeof(buf), &m, &pl_len));
}

/* ---------------------------------------------------------------------------
 * Test 2 — 32-byte TMAS round-trip.
 *
 * We *send* on TMA_TX (which writes a TMAS-tagged frame onto the
 * TMA_TX pipe) and verify that reading back via mock-inject onto
 * TMA_RX, then dma_recv_frame, recovers the identical 32 bytes.
 *
 * Because the mock pipes are per-channel (no internal cross-wiring),
 * we manually move the bytes from the TX channel's pipe to the RX
 * channel's pipe via dma_mock_inject. This mirrors the cosim bridge
 * the daemon uses on the real board.
 * ------------------------------------------------------------------------- */
static void test_send_recv_tmas_32b(void)
{
    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t) (0x40 + i);

    /* Send on TMA_TX → wraps with TMAS magic into the TMA_TX mock pipe. */
    TEST_ASSERT_EQUAL_INT(0, dma_send_frame(&g_ctx, DMA_CHAN_TMA_TX,
                                            payload, sizeof(payload)));

    /* Grab the framed bytes from TMA_TX's read-end via the IRQ FD,
     * then inject onto TMA_RX. We "cheat" using the IRQ-FD directly
     * because the test runs as the same process. */
    uint8_t framed[8 + 32];
    const int tx_fd = dma_get_irq_fd(&g_ctx, DMA_CHAN_TMA_TX);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, tx_fd);
    /* Read with retries — pipe write was atomic ≤ PIPE_BUF. */
    size_t got = 0;
    while (got < sizeof(framed)) {
        const ssize_t n = read(tx_fd, framed + got, sizeof(framed) - got);
        if (n <= 0) break;
        got += (size_t) n;
    }
    TEST_ASSERT_EQUAL_size_t(sizeof(framed), got);

    /* Verify the wire-format up-front (TMAS magic + length 32). */
    TEST_ASSERT_EQUAL_UINT8('T', framed[0]);
    TEST_ASSERT_EQUAL_UINT8('M', framed[1]);
    TEST_ASSERT_EQUAL_UINT8('A', framed[2]);
    TEST_ASSERT_EQUAL_UINT8('S', framed[3]);
    TEST_ASSERT_EQUAL_UINT8(0,   framed[4]);
    TEST_ASSERT_EQUAL_UINT8(0,   framed[5]);
    TEST_ASSERT_EQUAL_UINT8(0,   framed[6]);
    TEST_ASSERT_EQUAL_UINT8(32,  framed[7]);

    /* Inject onto TMA_RX and recv. */
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             framed, sizeof(framed)));

    uint8_t out[64];
    size_t  out_len = 0;
    const int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX,
                                  out, sizeof(out), &out_len, /*ms*/100);
    TEST_ASSERT_EQUAL_INT(32, rc);
    TEST_ASSERT_EQUAL_size_t(32, out_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 32);

    DmaStats st = (DmaStats) {0};
    dma_get_stats(&g_ctx, DMA_CHAN_TMA_RX, &st);
    TEST_ASSERT_EQUAL_UINT64(1, st.frames_recv_ok);
    TEST_ASSERT_EQUAL_UINT64(32, st.bytes_recv);
}

/* ---------------------------------------------------------------------------
 * Test 3 — TMDC voice round-trip on TMD_TX/RX (44 bytes).
 * ------------------------------------------------------------------------- */
static void test_send_recv_tmdc_voice(void)
{
    /* ARCHITECTURE.md says TmdSap voice is 274 bits MSB-aligned
     * = 35 bytes payload. We test with 44 to match the FPGA frame-
     * packer test in feat/a1-fpga-axi-dma which carries the
     * MSB-aligned 274-bit ACELP body padded out. */
    uint8_t payload[44];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t) i;

    TEST_ASSERT_EQUAL_INT(0, dma_send_frame(&g_ctx, DMA_CHAN_TMD_TX,
                                            payload, sizeof(payload)));

    uint8_t framed[8 + 44];
    const int tx_fd = dma_get_irq_fd(&g_ctx, DMA_CHAN_TMD_TX);
    size_t got = 0;
    while (got < sizeof(framed)) {
        const ssize_t n = read(tx_fd, framed + got, sizeof(framed) - got);
        if (n <= 0) break;
        got += (size_t) n;
    }
    TEST_ASSERT_EQUAL_size_t(sizeof(framed), got);

    TEST_ASSERT_EQUAL_UINT8('T', framed[0]);
    TEST_ASSERT_EQUAL_UINT8('M', framed[1]);
    TEST_ASSERT_EQUAL_UINT8('D', framed[2]);
    TEST_ASSERT_EQUAL_UINT8('C', framed[3]);

    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMD_RX,
                                             framed, sizeof(framed)));
    uint8_t out[64];
    size_t out_len = 0;
    const int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMD_RX,
                                  out, sizeof(out), &out_len, 100);
    TEST_ASSERT_EQUAL_INT(44, rc);
    TEST_ASSERT_EQUAL_size_t(44, out_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 44);
}

/* ---------------------------------------------------------------------------
 * Test 4 — TMAR (FPGA-side UMAC report) on TMA_RX.
 * Reports come *only* on RX; we craft the frame and inject directly.
 * ------------------------------------------------------------------------- */
static void test_recv_tmar_report(void)
{
    uint8_t payload[12];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t) (0xA0 + i);

    uint8_t framed[8 + 12];
    build_frame(framed, FRAME_MAGIC_TMAR, payload, 12);

    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             framed, sizeof(framed)));

    uint8_t out[32];
    size_t  out_len = 0;
    TEST_ASSERT_EQUAL_INT(12, dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX,
                                             out, sizeof(out), &out_len, 100));
    TEST_ASSERT_EQUAL_size_t(12, out_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 12);
}

/* ---------------------------------------------------------------------------
 * Test 5 — bad magic → -EBADMSG, drop counter increments, re-sync hunt.
 * ------------------------------------------------------------------------- */
static void test_recv_bad_magic_drops(void)
{
    /* 1) inject 4 bytes of trash followed by a valid TMAS frame. The
     *    bad-magic byte gets dropped one at a time; after 4 retries
     *    the parser locks onto the valid frame. */
    const uint8_t trash[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             trash, sizeof(trash)));
    uint8_t pl[4] = { 1, 2, 3, 4 };
    uint8_t framed[8 + 4];
    build_frame(framed, FRAME_MAGIC_TMAS, pl, 4);
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             framed, sizeof(framed)));

    /* The first call should -EBADMSG (bad magic 0xDE 0xAD 0xBE 0xEF). */
    uint8_t out[16];
    size_t  out_len = 0;
    int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX, out, sizeof(out),
                            &out_len, 100);
    TEST_ASSERT_EQUAL_INT(-EBADMSG, rc);

    /* Keep calling until we lock onto the valid frame (or 8 tries —
     * one per byte we might need to slide past). */
    int frames_got = 0;
    for (int i = 0; i < 8 && frames_got == 0; ++i) {
        rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX, out, sizeof(out),
                            &out_len, 50);
        if (rc > 0) {
            frames_got = 1;
            TEST_ASSERT_EQUAL_size_t(4, out_len);
            TEST_ASSERT_EQUAL_MEMORY(pl, out, 4);
        }
        /* otherwise rc may be -EBADMSG (more trash) or 0 (need data) */
    }
    TEST_ASSERT_EQUAL_INT(1, frames_got);

    DmaStats st = (DmaStats) {0};
    dma_get_stats(&g_ctx, DMA_CHAN_TMA_RX, &st);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(1, st.drop_bad_magic);
    TEST_ASSERT_EQUAL_UINT64(1, st.frames_recv_ok);
}

/* ---------------------------------------------------------------------------
 * Test 6 — length-mismatch on RX (declared > MAX → -EBADMSG, drop_bad_length).
 * ------------------------------------------------------------------------- */
static void test_recv_length_overflow(void)
{
    /* Build a header with valid magic but length = MAX+1. */
    uint8_t hdr[8];
    hdr[0]='T'; hdr[1]='M'; hdr[2]='A'; hdr[3]='S';
    const uint32_t bogus = DMA_FRAME_MAX_PAYLOAD + 1u;
    hdr[4] = (uint8_t)(bogus >> 24);
    hdr[5] = (uint8_t)(bogus >> 16);
    hdr[6] = (uint8_t)(bogus >>  8);
    hdr[7] = (uint8_t)(bogus >>  0);
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             hdr, sizeof(hdr)));

    uint8_t out[16];
    size_t  out_len = 0;
    const int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX, out, sizeof(out),
                                  &out_len, 50);
    TEST_ASSERT_EQUAL_INT(-EBADMSG, rc);

    DmaStats st = (DmaStats) {0};
    dma_get_stats(&g_ctx, DMA_CHAN_TMA_RX, &st);
    TEST_ASSERT_EQUAL_UINT64(1, st.drop_bad_length);
}

/* ---------------------------------------------------------------------------
 * Test 7 — partial frame split across 2 reads → reassembly correct.
 *
 * We inject the header in one chunk, recv (gets 0, no data extractable
 * yet), then inject the payload in a second chunk and recv again,
 * expecting the full frame.
 * ------------------------------------------------------------------------- */
static void test_recv_partial_reassembly(void)
{
    uint8_t payload[20];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(0x10 + i);

    uint8_t framed[8 + 20];
    build_frame(framed, FRAME_MAGIC_TMAS, payload, 20);

    /* Chunk 1: just the 8-byte header. */
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             framed, 8));

    uint8_t out[64];
    size_t  out_len = 0;
    int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX, out, sizeof(out),
                            &out_len, 50);
    TEST_ASSERT_EQUAL_INT(0, rc);  /* no payload yet */
    TEST_ASSERT_EQUAL_size_t(0, out_len);

    /* Chunk 2: the 20-byte payload. */
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             framed + 8, 20));
    rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX, out, sizeof(out),
                        &out_len, 50);
    TEST_ASSERT_EQUAL_INT(20, rc);
    TEST_ASSERT_EQUAL_size_t(20, out_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 20);
}

/* Variant: split across 3 chunks (4 hdr + 4 hdr + payload) — exercises
 * reasm buffer growing from 0 → 4 → 8 → 8+20. */
static void test_recv_three_chunk_reassembly(void)
{
    uint8_t payload[10] = { 0,1,2,3,4,5,6,7,8,9 };
    uint8_t framed[8 + 10];
    build_frame(framed, FRAME_MAGIC_TMDC, payload, 10);

    uint8_t out[32];
    size_t  out_len = 0;

    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMD_RX,
                                             framed, 4));
    TEST_ASSERT_EQUAL_INT(0, dma_recv_frame(&g_ctx, DMA_CHAN_TMD_RX,
                                            out, sizeof(out), &out_len, 50));

    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMD_RX,
                                             framed + 4, 4));
    TEST_ASSERT_EQUAL_INT(0, dma_recv_frame(&g_ctx, DMA_CHAN_TMD_RX,
                                            out, sizeof(out), &out_len, 50));

    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMD_RX,
                                             framed + 8, 10));
    const int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMD_RX,
                                  out, sizeof(out), &out_len, 50);
    TEST_ASSERT_EQUAL_INT(10, rc);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 10);
}

/* ---------------------------------------------------------------------------
 * Test 8 — timeout 0 with empty pipe → returns 0 (no error, no data).
 * ------------------------------------------------------------------------- */
static void test_recv_timeout_zero_empty(void)
{
    uint8_t out[16];
    size_t  out_len = 99;  /* sentinel: must be reset to 0 */
    const int rc = dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX,
                                  out, sizeof(out), &out_len, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* ---------------------------------------------------------------------------
 * Test 9 — back-to-back frames in one inject → both extract on
 * successive recv calls.
 * ------------------------------------------------------------------------- */
static void test_recv_back_to_back(void)
{
    uint8_t pl1[5]  = { 1,2,3,4,5 };
    uint8_t pl2[7]  = { 9,8,7,6,5,4,3 };
    uint8_t f1[8+5];
    uint8_t f2[8+7];
    build_frame(f1, FRAME_MAGIC_TMAS, pl1, 5);
    build_frame(f2, FRAME_MAGIC_TMAR, pl2, 7);

    uint8_t both[8+5+8+7];
    memcpy(both,         f1, sizeof(f1));
    memcpy(both + sizeof(f1), f2, sizeof(f2));
    TEST_ASSERT_EQUAL_INT(0, dma_mock_inject(&g_ctx, DMA_CHAN_TMA_RX,
                                             both, sizeof(both)));

    uint8_t out[32];
    size_t  out_len = 0;

    TEST_ASSERT_EQUAL_INT(5, dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX,
                                            out, sizeof(out), &out_len, 50));
    TEST_ASSERT_EQUAL_MEMORY(pl1, out, 5);

    /* Second frame must come out of the residue without another
     * inject — and even with timeout 0 (it is already in reasm). */
    TEST_ASSERT_EQUAL_INT(7, dma_recv_frame(&g_ctx, DMA_CHAN_TMA_RX,
                                            out, sizeof(out), &out_len, 0));
    TEST_ASSERT_EQUAL_MEMORY(pl2, out, 7);
}

/* ---------------------------------------------------------------------------
 * Test 10 — TX channel rejects on RX-only API and vice-versa.
 * ------------------------------------------------------------------------- */
static void test_send_on_rx_rejected(void)
{
    uint8_t pl[4] = { 0,1,2,3 };
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          dma_send_frame(&g_ctx, DMA_CHAN_TMA_RX, pl, 4));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          dma_send_frame(&g_ctx, DMA_CHAN_TMD_RX, pl, 4));
}

/* ---------------------------------------------------------------------------
 * Test 11 — irq_fd shape: returns >= 0 on opened channels, -EINVAL on bad. */
static void test_irq_fd_shape(void)
{
    for (int i = 0; i < DMA_CHAN_COUNT; ++i) {
        const int fd = dma_get_irq_fd(&g_ctx, (DmaChan) i);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    }
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          dma_get_irq_fd(&g_ctx, (DmaChan) DMA_CHAN_COUNT));
}

/* ---------------------------------------------------------------------------
 * Runner.
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_parse_header_happy);
    RUN_TEST(test_frame_parse_header_short_buf);
    RUN_TEST(test_frame_parse_header_bad_magic);
    RUN_TEST(test_frame_parse_header_len_mismatch);
    RUN_TEST(test_send_recv_tmas_32b);
    RUN_TEST(test_send_recv_tmdc_voice);
    RUN_TEST(test_recv_tmar_report);
    RUN_TEST(test_recv_bad_magic_drops);
    RUN_TEST(test_recv_length_overflow);
    RUN_TEST(test_recv_partial_reassembly);
    RUN_TEST(test_recv_three_chunk_reassembly);
    RUN_TEST(test_recv_timeout_zero_empty);
    RUN_TEST(test_recv_back_to_back);
    RUN_TEST(test_send_on_rx_rejected);
    RUN_TEST(test_irq_fd_shape);
    return UNITY_END();
}

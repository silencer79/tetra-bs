/* sw/dma_io/dma_io.c — userspace 4-channel DMA glue.
 *
 * Owned by S1 (S1-sw-dma-glue). Implements IF_DMA_API_v1.
 *
 * Two backends, gated by HAVE_XILINX_DMA:
 *
 *   HAVE_XILINX_DMA  defined → real backend over the in-tree Xilinx
 *                   `xilinx_dma.ko` already shipped on Board #1 (cf.
 *                   HARDWARE.md §4, no 3rd-party vendoring). Channel
 *                   device paths come from the DT overlay in
 *                   `dts/tetra_axi_dma_overlay.dtsi`:
 *                   /dev/dma_proxy_tma-sap-rx (or equivalent xilinx_dma
 *                   char-dev — confirm in-situ in Phase 3), …
 *
 *   HAVE_XILINX_DMA  undefined → pipe-pair mock for x86 host unit-tests.
 *                   Each channel gets one pipe-pair. dma_send_frame
 *                   writes the framed bytes into the pipe; dma_recv_frame
 *                   reads from the pipe and parses magic+length. A
 *                   test-only hook dma_mock_inject (declared in
 *                   dma_io_internal.h) writes raw bytes into the RX-side
 *                   of a channel's pipe so tests can stimulate RX
 *                   without going through the framer.
 *
 * Frame layout on the wire (matches docs/ARCHITECTURE.md §FPGA↔SW
 * Boundary and the FPGA frame packer/unpacker test in
 * feat/a1-fpga-axi-dma):
 *
 *     +----------+----------+----------+
 *     | MAGIC[4] | LEN_BE[4]| PAYLOAD  |
 *     +----------+----------+----------+
 *
 * MAGIC is the 4-byte ASCII tag stored in network/big-endian order
 * (so the bytes 'T','M','A','S' in that order on the wire), LEN_BE is
 * the payload byte-count in big-endian. Total frame = 8 + LEN bytes.
 *
 * Reassembly: dma_recv_frame is robust against partial-read short
 * returns from read(2). Internal per-channel staging buffer accumulates
 * bytes until a full frame is decoded; partial residue across calls is
 * preserved.
 */

#define _POSIX_C_SOURCE 200809L

#include "tetra/dma_io.h"
#include "dma_io_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * DT label defaults — must match dts/tetra_axi_dma_overlay.dtsi from
 * feat/a1-fpga-axi-dma and HARDWARE.md §4 channel naming. Only
 * referenced by the real-HW backend, so guarded to avoid -Wunused
 * on the host (HAVE_XILINX_DMA-undef) build.
 * ------------------------------------------------------------------------- */
#ifdef HAVE_XILINX_DMA
static const char *const DEFAULT_DT_LABELS[DMA_CHAN_COUNT] = {
    [DMA_CHAN_TMA_RX] = "tma-sap-rx",
    [DMA_CHAN_TMA_TX] = "tma-sap-tx",
    [DMA_CHAN_TMD_RX] = "tmd-sap-rx",
    [DMA_CHAN_TMD_TX] = "tmd-sap-tx",
};
#endif

/* TX-direction magics. RX magics are FPGA-emitted (TMAS for signalling
 * payload, TMAR for UMAC reports, TMDC for voice) and the SW side only
 * parses them on RX; for TX, the daemon writes signalling on TMA_TX
 * (always TMAS) and voice on TMD_TX (always TMDC). */
static const FrameMagic TX_MAGIC[DMA_CHAN_COUNT] = {
    [DMA_CHAN_TMA_RX] = (FrameMagic) 0,  /* unused */
    [DMA_CHAN_TMA_TX] = FRAME_MAGIC_TMAS,
    [DMA_CHAN_TMD_RX] = (FrameMagic) 0,  /* unused */
    [DMA_CHAN_TMD_TX] = FRAME_MAGIC_TMDC,
};

/* ---------------------------------------------------------------------------
 * Per-channel state lives in the public header (DmaChanState_, prefixed
 * with `_` to discourage direct access). We typedef shorter aliases for
 * readability inside this TU. The reassembly capacity is set in the
 * header (DMA_REASM_CAP_BYTES = 8 + DMA_FRAME_MAX_PAYLOAD).
 * ------------------------------------------------------------------------- */
typedef DmaChanState_ DmaChanState;
#define DMA_REASM_CAP DMA_REASM_CAP_BYTES

/* Field-access shorthand. Map the underscore-prefixed struct members
 * back onto the names this implementation was originally written
 * against. */
#define pipe_rd          _pipe_rd
#define pipe_wr          _pipe_wr
#define axidma_chan_id   _axidma_chan_id
#define axidma_buf       _axidma_buf
#define axidma_buf_cap   _axidma_buf_cap
#define reasm            _reasm
#define reasm_used       _reasm_used
#define opened           _opened
#define using_mock       _using_mock
#define chans            _chans

/* ---------------------------------------------------------------------------
 * Big-endian helpers — kept local; not worth pulling in <endian.h>.
 * ------------------------------------------------------------------------- */
static uint32_t be32_load(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24)
         | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] <<  8)
         | ((uint32_t) p[3] <<  0);
}

static void be32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >>  8);
    p[3] = (uint8_t) (v >>  0);
}

/* ---------------------------------------------------------------------------
 * Frame magic helpers.
 * ------------------------------------------------------------------------- */
bool frame_magic_is_known(uint32_t magic)
{
    return magic == FRAME_MAGIC_TMAS
        || magic == FRAME_MAGIC_TMAR
        || magic == FRAME_MAGIC_TMDC;
}

int frame_parse_header(const uint8_t *buf,
                       size_t          len,
                       FrameMagic     *out_magic,
                       uint32_t       *out_payload_len)
{
    if (buf == NULL || out_magic == NULL || out_payload_len == NULL) {
        return -EINVAL;
    }
    if (len < DMA_FRAME_HEADER_BYTES) {
        return -EBADMSG;
    }

    const uint32_t magic_raw = be32_load(buf);
    if (!frame_magic_is_known(magic_raw)) {
        return -EBADMSG;
    }

    const uint32_t payload_len = be32_load(buf + 4);
    if (payload_len > DMA_FRAME_MAX_PAYLOAD) {
        return -EBADMSG;
    }
    /* Caller-known byte-count must contain the declared payload — we
     * are checking the *parser* invariant, not "data already arrived".
     * Reassembler enforces the latter via reasm_used. */
    if (payload_len > len - DMA_FRAME_HEADER_BYTES) {
        return -EBADMSG;
    }

    *out_magic       = (FrameMagic) magic_raw;
    *out_payload_len = payload_len;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Mock-backend helpers. All ifdef'd into existence; the real-HW build
 * elides this block.
 * ------------------------------------------------------------------------- */
#ifndef HAVE_XILINX_DMA

static int mock_open_chan(DmaChanState *s)
{
    int fds[2];
    if (pipe(fds) < 0) {
        return -errno;
    }
    /* Set both ends non-blocking — recv loop drives blocking via poll(). */
    for (int i = 0; i < 2; ++i) {
        const int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            const int e = errno;
            close(fds[0]);
            close(fds[1]);
            return -e;
        }
    }
    s->pipe_rd = fds[0];
    s->pipe_wr = fds[1];
    s->opened  = true;
    return 0;
}

static void mock_close_chan(DmaChanState *s)
{
    if (!s->opened) {
        return;
    }
    if (s->pipe_rd >= 0) {
        close(s->pipe_rd);
        s->pipe_rd = -1;
    }
    if (s->pipe_wr >= 0) {
        close(s->pipe_wr);
        s->pipe_wr = -1;
    }
    s->opened = false;
}

/* Test hook: inject raw bytes into a channel's pipe so dma_recv_frame
 * sees them. Bytes go in via the *write* end of the same pipe whose
 * read end dma_recv_frame consumes. */
int dma_mock_inject(DmaCtx *ctx, DmaChan ch, const uint8_t *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) return -EINVAL;
    if (ch >= DMA_CHAN_COUNT)        return -EINVAL;
    if (!ctx->using_mock)            return -ENOSYS;

    DmaChanState *s = &ctx->chans[(int) ch];
    if (!s->opened) return -EBADF;

    size_t off = 0;
    while (off < len) {
        const ssize_t n = write(s->pipe_wr, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        off += (size_t) n;
    }
    return 0;
}

#endif /* !HAVE_XILINX_DMA */

/* ---------------------------------------------------------------------------
 * Real-HW backend skeleton — only compiles when HAVE_XILINX_DMA is defined.
 * Target: in-tree `xilinx_dma.ko` already shipped on Board #1 under
 * `/root/kernel_modules32/` (HARDWARE.md §4 carry-over from the openwifi
 * stack; matches `xlnx,axi-dma-1.00.a` device-tree compatible). No 3rd-party
 * library, no vendoring step.
 *
 * The host CI build path leaves HAVE_XILINX_DMA undefined, so this block
 * is elided; the pipe-mock backend above is what unit-tests exercise.
 *
 * Real-HW implementation TODO (Phase-3): open `/dev/dma_proxy_*` (or
 * equivalent xilinx_dma char-dev path on Kuiper) per DT label, ioctl-based
 * start + mmap'd DMA buffer pool. Header below is a placeholder until the
 * board's actual char-dev shape is confirmed in-situ.
 * ------------------------------------------------------------------------- */
#ifdef HAVE_XILINX_DMA
#include <xilinx_dma.h>  /* TODO: replace with /dev/dma_proxy ioctl headers */

static axidma_dev_t g_axidma_dev;  /* singleton handle from axidma_init() */
static int          g_axidma_ref;  /* refcount across DmaCtx lifetimes    */

static int real_open_chan(DmaChanState *s, const char *dt_label)
{
    /* xilinx_dma binds channels by integer device-id (DT property
     * xlnx,device-id). The DT overlay in feat/a1-fpga-axi-dma assigns:
     *   tma-sap-rx → 0, tma-sap-tx → 1, tmd-sap-rx → 2, tmd-sap-tx → 3.
     * We resolve dt_label → id via a static table; future work could
     * re-read /sys/firmware/devicetree/base if needed. */
    static const struct { const char *lbl; int id; } LBL2ID[] = {
        { "tma-sap-rx", 0 }, { "tma-sap-tx", 1 },
        { "tmd-sap-rx", 2 }, { "tmd-sap-tx", 3 },
    };
    int id = -1;
    for (size_t i = 0; i < sizeof(LBL2ID) / sizeof(LBL2ID[0]); ++i) {
        if (strcmp(LBL2ID[i].lbl, dt_label) == 0) { id = LBL2ID[i].id; break; }
    }
    if (id < 0) return -ENODEV;

    if (g_axidma_ref++ == 0) {
        g_axidma_dev = axidma_init();
        if (g_axidma_dev == NULL) { g_axidma_ref = 0; return -EIO; }
    }

    s->axidma_chan_id  = id;
    s->axidma_buf_cap  = DMA_REASM_CAP;
    s->axidma_buf      = axidma_malloc(g_axidma_dev, s->axidma_buf_cap);
    if (s->axidma_buf == NULL) return -ENOMEM;

    s->opened = true;
    return 0;
}

static void real_close_chan(DmaChanState *s)
{
    if (!s->opened) return;
    if (s->axidma_buf) {
        axidma_free(g_axidma_dev, s->axidma_buf, s->axidma_buf_cap);
        s->axidma_buf = NULL;
    }
    s->opened = false;
    if (--g_axidma_ref == 0 && g_axidma_dev != NULL) {
        axidma_destroy(g_axidma_dev);
        g_axidma_dev = NULL;
    }
}
#endif /* HAVE_XILINX_DMA */

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

int dma_init(DmaCtx *ctx, const DmaCfg *cfg)
{
    if (ctx == NULL) return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));
    for (int i = 0; i < DMA_CHAN_COUNT; ++i) {
        ctx->chans[i].pipe_rd = -1;
        ctx->chans[i].pipe_wr = -1;
    }

    /* Decide backend. Mock if either: cfg->force_mock, or we were not
     * built with HAVE_XILINX_DMA. The host x86 unit-test build is the
     * latter. */
    bool use_mock = (cfg != NULL && cfg->force_mock);
#ifndef HAVE_XILINX_DMA
    use_mock = true;
#endif
    ctx->using_mock = use_mock;

    for (int i = 0; i < DMA_CHAN_COUNT; ++i) {
        DmaChanState *s = &ctx->chans[i];
        int rc;
        if (use_mock) {
#ifndef HAVE_XILINX_DMA
            rc = mock_open_chan(s);
#else
            rc = -ENOSYS;  /* xilinx_dma build requested mock at runtime —
                              not supported (would require re-building
                              without xilinx_dma). */
#endif
        } else {
#ifdef HAVE_XILINX_DMA
            const char *lbl = (cfg != NULL && cfg->chan_dt_labels[i] != NULL)
                            ? cfg->chan_dt_labels[i]
                            : DEFAULT_DT_LABELS[i];
            rc = real_open_chan(s, lbl);
#else
            rc = -ENOSYS;
#endif
        }
        if (rc != 0) {
            /* Roll back any partially-opened channels so the caller
             * sees an all-or-nothing init. */
            for (int j = 0; j < i; ++j) {
#ifndef HAVE_XILINX_DMA
                mock_close_chan(&ctx->chans[j]);
#else
                real_close_chan(&ctx->chans[j]);
#endif
            }
            return rc;
        }
    }
    return 0;
}

void dma_close(DmaCtx *ctx)
{
    if (ctx == NULL) return;
    for (int i = 0; i < DMA_CHAN_COUNT; ++i) {
#ifndef HAVE_XILINX_DMA
        mock_close_chan(&ctx->chans[i]);
#else
        real_close_chan(&ctx->chans[i]);
#endif
    }
}

int dma_get_irq_fd(DmaCtx *ctx, DmaChan ch)
{
    if (ctx == NULL || ch >= DMA_CHAN_COUNT) return -EINVAL;
    DmaChanState *s = &ctx->chans[(int) ch];
    if (!s->opened) return -EBADF;
#ifndef HAVE_XILINX_DMA
    return s->pipe_rd;
#else
    /* xilinx_dma has no per-channel IRQ FD; the daemon polls via the
     * char-dev FD instead. Caller must axidma_set_callback() for the
     * full lifecycle. */
    return -ENOSYS;
#endif
}

int dma_get_stats(const DmaCtx *ctx, DmaChan ch, DmaStats *out)
{
    if (ctx == NULL || out == NULL || ch >= DMA_CHAN_COUNT) {
        return -EINVAL;
    }
    const DmaChanState *s = &ctx->chans[(int) ch];
    out->frames_recv_ok  = s->_frames_recv_ok;
    out->frames_send_ok  = s->_frames_send_ok;
    out->bytes_recv      = s->_bytes_recv;
    out->bytes_send      = s->_bytes_send;
    out->drop_bad_magic  = s->_drop_bad_magic;
    out->drop_bad_length = s->_drop_bad_length;
    out->drop_short_read = s->_drop_short_read;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Send path. TX channels only.
 * ------------------------------------------------------------------------- */
int dma_send_frame(DmaCtx        *ctx,
                   DmaChan        ch,
                   const uint8_t *buf,
                   size_t         len)
{
    if (ctx == NULL || buf == NULL)               return -EINVAL;
    if (ch >= DMA_CHAN_COUNT)                     return -EINVAL;
    if (dma_chan_is_rx(ch))                       return -EINVAL;
    if (len > DMA_FRAME_MAX_PAYLOAD)              return -EMSGSIZE;

    DmaChanState *s = &ctx->chans[(int) ch];
    if (!s->opened) return -EBADF;

    uint8_t hdr[DMA_FRAME_HEADER_BYTES];
    be32_store(hdr,     (uint32_t) TX_MAGIC[ch]);
    be32_store(hdr + 4, (uint32_t) len);

#ifndef HAVE_XILINX_DMA
    /* Two writes; pipes are atomic up to PIPE_BUF (4096 on Linux),
     * so as long as len ≤ DMA_FRAME_MAX_PAYLOAD = 4096 the payload
     * write is a single syscall — no interleaving with concurrent
     * writers from the same process. */
    size_t off;

    off = 0;
    while (off < sizeof(hdr)) {
        const ssize_t n = write(s->pipe_wr, hdr + off, sizeof(hdr) - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        off += (size_t) n;
    }

    off = 0;
    while (off < len) {
        const ssize_t n = write(s->pipe_wr, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        off += (size_t) n;
    }
#else
    /* Real backend: copy hdr+payload into the DMA-coherent buffer and
     * fire a one-way transfer. */
    if (len + sizeof(hdr) > s->axidma_buf_cap) return -EMSGSIZE;
    memcpy(s->axidma_buf, hdr, sizeof(hdr));
    memcpy((uint8_t *) s->axidma_buf + sizeof(hdr), buf, len);
    int rc = axidma_oneway_transfer(g_axidma_dev,
                                    s->axidma_chan_id,
                                    s->axidma_buf,
                                    len + sizeof(hdr),
                                    /*wait=*/true);
    if (rc < 0) return rc;
#endif

    s->_frames_send_ok += 1;
    s->_bytes_send     += len;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Receive path with reassembly. The reasm buffer holds bytes carried
 * over from previous calls (a frame split across reads, or the leading
 * bytes of the *next* frame appended to a complete one). dma_recv_frame
 * appends new bytes from one read and tries to extract one frame.
 * ------------------------------------------------------------------------- */

/* Read once into the staging buffer. Returns:
 *    > 0  bytes appended
 *      0  no data (EAGAIN/EWOULDBLOCK, EOF on pipe → still 0 here, caller
 *         differentiates by checking eof_seen via a separate path)
 *    < 0  -errno
 */
static ssize_t reasm_read_one(DmaChanState *s)
{
#ifndef HAVE_XILINX_DMA
    if (s->reasm_used >= sizeof(s->reasm)) return 0;
    const size_t free = sizeof(s->reasm) - s->reasm_used;
    const ssize_t n   = read(s->pipe_rd, s->reasm + s->reasm_used, free);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR)                         return 0;
        return -errno;
    }
    if (n == 0) return 0;  /* EOF on pipe — treat as no-data this round */
    s->reasm_used += (size_t) n;
    return n;
#else
    (void) s;
    return -ENOSYS;
#endif
}

/* Try to extract one frame from the staging buffer into (out, cap).
 * Returns:
 *    1   one frame extracted; *out_len set, reasm_used compacted.
 *    0   not enough bytes yet — caller must keep reading.
 *   <0   -EBADMSG: malformed frame (drop accounted in stats); reasm
 *        buffer is advanced past the bad header so the next call may
 *        re-sync.
 */
static int reasm_try_extract(DmaChanState *s,
                             uint8_t      *out,
                             size_t        cap,
                             size_t       *out_len)
{
    if (s->reasm_used < DMA_FRAME_HEADER_BYTES) return 0;

    const uint32_t magic = be32_load(s->reasm);
    if (!frame_magic_is_known(magic)) {
        /* Drop one byte and retry on next call — re-sync hunt. */
        s->_drop_bad_magic += 1;
        memmove(s->reasm, s->reasm + 1, s->reasm_used - 1);
        s->reasm_used -= 1;
        return -EBADMSG;
    }
    const uint32_t plen = be32_load(s->reasm + 4);
    if (plen > DMA_FRAME_MAX_PAYLOAD) {
        s->_drop_bad_length += 1;
        /* Header was syntactically valid but length out of range —
         * skip the whole declared "frame" is not safe (length bogus);
         * skip just the 8-byte header and resync. */
        memmove(s->reasm,
                s->reasm + DMA_FRAME_HEADER_BYTES,
                s->reasm_used - DMA_FRAME_HEADER_BYTES);
        s->reasm_used -= DMA_FRAME_HEADER_BYTES;
        return -EBADMSG;
    }
    const size_t total = DMA_FRAME_HEADER_BYTES + (size_t) plen;
    if (s->reasm_used < total) {
        return 0;  /* need more bytes */
    }
    if (cap < (size_t) plen) {
        /* Caller's buffer too small — surface and DO NOT consume,
         * so the next call with a bigger buffer can succeed. */
        return -ENOSPC;
    }

    if (plen > 0) {
        memcpy(out, s->reasm + DMA_FRAME_HEADER_BYTES, plen);
    }
    *out_len = plen;

    const size_t leftover = s->reasm_used - total;
    if (leftover > 0) {
        memmove(s->reasm, s->reasm + total, leftover);
    }
    s->reasm_used = leftover;

    s->_frames_recv_ok += 1;
    s->_bytes_recv     += plen;
    return 1;
}

int dma_recv_frame(DmaCtx  *ctx,
                   DmaChan  ch,
                   uint8_t *buf,
                   size_t   cap,
                   size_t  *out_len,
                   int      timeout_ms)
{
    if (ctx == NULL || buf == NULL || out_len == NULL) return -EINVAL;
    if (ch >= DMA_CHAN_COUNT)                          return -EINVAL;
    *out_len = 0;

    DmaChanState *s = &ctx->chans[(int) ch];
    if (!s->opened) return -EBADF;

    /* Step 1: try to satisfy from existing staging buffer. */
    int rc = reasm_try_extract(s, buf, cap, out_len);
    if (rc == 1)         return (int) *out_len;
    if (rc == -ENOSPC)   return -ENOSPC;
    if (rc == -EBADMSG)  return -EBADMSG;
    /* rc == 0 → fall through to the read loop. */

#ifndef HAVE_XILINX_DMA
    /* Step 2: poll once with the requested timeout. timeout_ms == 0 →
     * non-blocking poll-once; <0 → block forever. */
    struct pollfd pfd = { .fd = s->pipe_rd, .events = POLLIN, .revents = 0 };

    /* Edge case: we may already have residue in the staging buffer
     * (partial frame). In that case we must keep reading until either
     * a frame extracts, the pipe drains, or timeout elapses. */
    int remaining = timeout_ms;
    for (;;) {
        const int pr = poll(&pfd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR) {
                if (remaining > 0) {
                    /* poll(2) returns -EINTR without updating timeout;
                     * treat as one timeout-tick consumed and continue
                     * with the same remaining. Conservative — could
                     * over-wait by one signal-arrival, acceptable in
                     * unit tests. */
                    continue;
                }
                continue;
            }
            return -errno;
        }
        if (pr == 0) {
            /* Timeout. If we have residue, surface as 0 (no full
             * frame); the residue persists for the next call. */
            return 0;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Pipe partner closed — drain whatever is left and return. */
            if (pfd.revents & POLLIN) {
                /* fall through to read */
            } else {
                return 0;
            }
        }

        const ssize_t nr = reasm_read_one(s);
        if (nr < 0) return (int) nr;
        if (nr == 0) {
            /* Spurious wakeup or EOF with empty pipe — treat as no
             * progress and keep waiting if time remains. */
            if (timeout_ms == 0) return 0;
            continue;
        }
        rc = reasm_try_extract(s, buf, cap, out_len);
        if (rc == 1)         return (int) *out_len;
        if (rc == -ENOSPC)   return -ENOSPC;
        if (rc == -EBADMSG)  return -EBADMSG;
        /* rc == 0 → still need more bytes; loop. */

        if (timeout_ms == 0) {
            /* Single-shot mode: do not loop on the same call. */
            return 0;
        }
    }
#else
    /* Real-HW path: blocking axidma_oneway_transfer with timeout
     * approximated by axidma_set_callback + condvar — out of scope
     * for the S1 stub; will be filled in during Phase-3 cosim integ. */
    (void) timeout_ms;
    return -ENOSYS;
#endif
}
